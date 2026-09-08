#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "codegen.h"
#include "vo_rt.h"
#include "memstream_compat.h"
#include "parser.h"
#include "ast.h"

/* =========================================================================
 * codegen.c - AST -> real x86-64 machine instructions.
 *
 * Every VO statement/expression is lowered here, at compile time, into
 * concrete instruction sequences (mov/lea/cmp/jmp/call/...). The only
 * things ever "called" at run time are the small, AST-agnostic helpers
 * in vo_rt.c (arithmetic, printing, etc.) - see the note at the top of
 * vo_rt.h. There is no bytecode dispatch loop and no ASTNode pointer
 * anywhere in the emitted binary.
 *
 * Value representation: every VO value that needs an address (to be
 * passed to a vo_rt_* helper) lives in a `VoValue`-sized slot - either
 * a compile-time-allocated global (.bss) for a declared VAR/CONST/VMA,
 * or a stack slot inside the current function's frame for expression
 * temporaries. Slot *contents* are only ever touched through vo_rt
 * calls, so codegen never needs to know VoValue's field layout, only
 * its size (sizeof(VoValue), pulled in via vo_rt.h).
 *
 * Known v1 limitations (clearly reported as compile errors rather than
 * silently mis-compiled): WHEN/ENDWHEN handlers (event queue + edge
 * triggering) and OLD_VALUE/NEW_VALUE are not lowered yet - use the
 * tree-walking interpreter (voi) for programs that need them.
 * ========================================================================= */

#define SLOT_SIZE ((long)sizeof(VoValue))

/* ---- calling convention ------------------------------------------------ */
typedef struct {
    const char *argreg[4]; /* enough - no vo_rt call takes more than 4 args */
    int shadow_space;      /* extra permanent stack reserved for callee (Windows) */
} ABI;

static ABI abi_for(CgTarget t) {
    ABI a;
    if (t == CG_TARGET_WINDOWS) {
        a.argreg[0] = "%rcx"; a.argreg[1] = "%rdx"; a.argreg[2] = "%r8"; a.argreg[3] = "%r9";
        a.shadow_space = 32;
    } else {
        a.argreg[0] = "%rdi"; a.argreg[1] = "%rsi"; a.argreg[2] = "%rdx"; a.argreg[3] = "%rcx";
        a.shadow_space = 0;
    }
    return a;
}

/* ---- compile-time VMA allocator (mirrors analyzer.c Sec 6.A/6.B) ------- */
#define VMA_NUMBERS_PER_LETTER 9999
typedef struct { long index; char address[16]; int allocated; } CVmaSlot;
typedef struct { CVmaSlot *slots; int count, capacity; } CVmaTable;

static long letters_to_index(const char *s, int len) {
    long idx = 0;
    for (int i = 0; i < len; i++) idx = idx * 26 + (s[i] - 'A' + 1);
    return idx;
}
static long vma_canonical_index(const char *vma) {
    int i = 0; while (isupper((unsigned char)vma[i])) i++;
    long letter_idx = letters_to_index(vma, i);
    long number = strtol(vma + i, NULL, 10);
    return (letter_idx - 1) * VMA_NUMBERS_PER_LETTER + number;
}
static void index_to_vma(long index, char *out) {
    long number = ((index - 1) % VMA_NUMBERS_PER_LETTER) + 1;
    long letter_idx = ((index - 1) / VMA_NUMBERS_PER_LETTER) + 1;
    char letters[16]; int li = 0;
    while (letter_idx > 0) { long rem = (letter_idx - 1) % 26; letters[li++] = (char)('A' + rem); letter_idx = (letter_idx - 1) / 26; }
    int oi = 0; for (int i = li - 1; i >= 0; i--) out[oi++] = letters[i];
    sprintf(out + oi, "%ld", number);
}
static CVmaSlot *vma_slot_for_index(CVmaTable *t, long idx) {
    for (int i = 0; i < t->count; i++) if (t->slots[i].index == idx) return &t->slots[i];
    if (t->count == t->capacity) { t->capacity = t->capacity ? t->capacity * 2 : 8; t->slots = realloc(t->slots, sizeof(CVmaSlot) * t->capacity); }
    CVmaSlot *slot = &t->slots[t->count++];
    memset(slot, 0, sizeof(*slot)); slot->index = idx; index_to_vma(idx, slot->address);
    return slot;
}
static CVmaSlot *vma_alloc_next(CVmaTable *t) {
    long idx = 1;
    for (;;) {
        CVmaSlot *existing = NULL;
        for (int i = 0; i < t->count; i++) if (t->slots[i].index == idx) { existing = &t->slots[i]; break; }
        if (!existing || !existing->allocated) break;
        idx++;
    }
    CVmaSlot *slot = vma_slot_for_index(t, idx);
    slot->allocated = 1;
    return slot;
}
static CVmaSlot *vma_alloc_specific(CVmaTable *t, const char *addr) {
    long idx = vma_canonical_index(addr);
    CVmaSlot *slot = vma_slot_for_index(t, idx);
    if (slot->allocated) return NULL;
    slot->allocated = 1;
    return slot;
}

/* ---- global .bss VMA slot registry (dedup by canonical address) ------- */
typedef struct { char addr[16]; char label[40]; } GVma;

/* ---- compile-time symbol table ---------------------------------------- */
typedef struct {
    char *name;
    int is_loop_var;
    char vma[16];        /* backing VMA address, for normal VAR/CONST     */
    char loop_label[40]; /* backing global label, for FOR loop iterators  */
    int autocleans;
} CSym;
typedef struct CScope {
    CSym *syms; int count, cap;
    struct CScope *parent;
} CScope;

static CScope *scope_push(CScope *parent) { CScope *s = calloc(1, sizeof(*s)); s->parent = parent; return s; }
static CSym *scope_find_local(CScope *s, const char *name) {
    for (int i = 0; i < s->count; i++) if (strcmp(s->syms[i].name, name) == 0) return &s->syms[i];
    return NULL;
}
static CSym *scope_resolve(CScope *s, const char *name) {
    for (; s; s = s->parent) { CSym *sym = scope_find_local(s, name); if (sym) return sym; }
    return NULL;
}
static CSym *scope_declare(CScope *s, const char *name) {
    if (s->count == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->syms = realloc(s->syms, sizeof(CSym) * s->cap); }
    CSym *sym = &s->syms[s->count++];
    memset(sym, 0, sizeof(*sym));
    sym->name = strdup(name);
    return sym;
}

/* ---- the code generator state ------------------------------------------ */

/* v1.3 jobs: each JOB becomes its own emitted function. */
typedef struct {
    ASTNode *decl;          /* the NODE_JOB_DECL */
    int id;
    int tab_index;          /* position in cg->jobs[] == index in vo_job_tab */
    int param_count;
    char param_labels[16][40]; /* .bss slot for each parameter */
    char ret_label[40];        /* .bss slot holding the job's return value */
    char *name;              /* lookup key: plain "ADD" for top-level jobs,
                                 "math#ADD" for jobs declared inside a PEICE -
                                 keeps module jobs from colliding with each
                                 other or with top-level jobs of the same name */
    char *module_name;       /* owning module's name, or NULL for top-level jobs */
} CJob;

/* ---- module system (native compiler) ------------------------------------ */
typedef enum { CG_MOD_MEMBER_JOB, CG_MOD_MEMBER_VAR } CgModMemberKind;

typedef struct {
    char *name;                /* exported name (e.g., "ADD") */
    CgModMemberKind kind;      /* job or variable */
    char *job_internal;        /* for jobs: internal job name (e.g., "math#ADD") */
    char var_label[40];        /* for vars: .bss slot label */
    int is_exported;           /* SHIPped? */
} CgModMember;

typedef struct {
    char *name;                /* module name (e.g., "math") */
    CgModMember *members; int member_count, member_cap;
    int loaded;                /* 1 if fully loaded */
    int loading;               /* 1 if currently being loaded (for cycle detection) */
    ASTNode *peice_ast;        /* the PEICE_DECL node for this module (for compilation) */
} CgModule;

/* nested DO/GRABE contexts for lowering SERVE/DEMAND branches */

/* nested DO/GRABE contexts for lowering SERVE/DEMAND branches */
typedef struct {
    const char *flag_label;    /* .bss VoValue flag test field (yn at +8) */
    int end_label;             /* jump here on issue (end of try block) */
} CDo;

typedef struct {
    FILE *body;          /* memstream: current function body instructions */
    char *body_buf; size_t body_len;
    FILE *rodata;         /* memstream: string literal constants          */
    char *rodata_buf; size_t rodata_len;
    CgTarget target;
    ABI abi;
    int depth, max_depth; /* expr temp-slot stack (LIFO) - current function */
    int label_id;
    int str_id;
    int for_id;
    CScope *scope;
    CVmaTable vmas;
    GVma *globals; int gcount, gcap;
    int had_error;
    int autoclean_on;

    CJob *jobs; int job_count, job_cap;
    int cur_job_id;             /* -1 => top-level vo_main body */
    const char *cur_job_ret;    /* return .bss label for the job being emitted */
    const char *cur_job_module; /* owning module name while emitting a module job's body, else NULL */

    /* Module system (native) */
    CgModule *modules; int module_count, module_cap;
    const char *source_dir;     /* directory of main source for BRING resolution */

    CDo *do_stack; int do_depth, do_cap;
    int max_stack_args;     /* most stack args passed by any call (win64 spill area) */
} CG;

static void cg_error(CG *cg, int line, const char *msg) {
    fprintf(stderr, "[line %d] Compile error: %s\n", line, msg);
    cg->had_error = 1;
}

/* ---- job registry (name -> CJob) ---- */
static CJob *cg_job_lookup(CG *cg, const char *name) {
    for (int i = 0; i < cg->job_count; i++) {
        if (cg->jobs[i].name && strcmp(cg->jobs[i].name, name) == 0) return &cg->jobs[i];
    }
    return NULL;
}

/* `lookup_name` is the key emit_call_expr will find this job under - plain
   job name for top-level jobs, "module#job" for jobs declared inside a
   PEICE (see cg_job_lookup callers). `module_name` is NULL for top-level
   jobs, else the owning module's name (used to resolve unqualified calls
   made from inside that same module - see emit_call_expr). */
static CJob *cg_job_add(CG *cg, ASTNode *decl, int id, const char *lookup_name, const char *module_name) {
    if (cg_job_lookup(cg, lookup_name)) return NULL; /* dup - analyzer catches for top-level */
    if (cg->job_count == cg->job_cap) {
        cg->job_cap = cg->job_cap ? cg->job_cap * 2 : 8;
        cg->jobs = realloc(cg->jobs, sizeof(CJob) * cg->job_cap);
    }
    CJob *j = &cg->jobs[cg->job_count++];
    j->decl = decl;
    j->id = id;
    j->tab_index = cg->job_count - 1;
    j->name = strdup(lookup_name);
    j->module_name = module_name ? strdup(module_name) : NULL;
    return j;
}

/* ---- module system helpers (native) ---- */
static CgModule *cg_module_find(CG *cg, const char *name) {
    for (int i = 0; i < cg->module_count; i++)
        if (strcmp(cg->modules[i].name, name) == 0)
            return &cg->modules[i];
    return NULL;
}

static CgModule *cg_module_ensure(CG *cg, const char *name) {
    CgModule *m = cg_module_find(cg, name);
    if (m) return m;
    if (cg->module_count == cg->module_cap) {
        cg->module_cap = cg->module_cap ? cg->module_cap * 2 : 8;
        cg->modules = realloc(cg->modules, sizeof(CgModule) * cg->module_cap);
    }
    CgModule *nm = &cg->modules[cg->module_count++];
    memset(nm, 0, sizeof(CgModule));
    nm->name = strdup(name);
    return nm;
}

static CgModMember *cg_module_member_find(CgModule *mod, const char *name) {
    for (int i = 0; i < mod->member_count; i++)
        if (strcmp(mod->members[i].name, name) == 0)
            return &mod->members[i];
    return NULL;
}

static CgModMember *cg_module_member_add(CgModule *mod, const char *name, CgModMemberKind kind) {
    if (mod->member_count == mod->member_cap) {
        mod->member_cap = mod->member_cap ? mod->member_cap * 2 : 8;
        mod->members = realloc(mod->members, sizeof(CgModMember) * mod->member_cap);
    }
    CgModMember *mm = &mod->members[mod->member_count++];
    memset(mm, 0, sizeof(CgModMember));
    mm->name = strdup(name);
    mm->kind = kind;
    return mm;
}

/* Load a module from a .vo file at the given path, relative to base_dir.
   Returns 0 on success, 1 on error (error reported via cg_error). */
static int cg_module_load(CG *cg, const char *path, const char *base_dir) {
    char errbuf[256];
    /* Resolve full path */
    char full_path[4096];
    if (path[0] == '/' || (path[0] && path[1] == ':')) {
        snprintf(full_path, sizeof(full_path), "%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, path);
    }

    /* Read file */
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        snprintf(errbuf, sizeof(errbuf), "module file not found: %s", full_path);
        cg_error(cg, 0, errbuf);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(size + 1);
    if (fread(source, 1, size, f) != (size_t)size) { free(source); fclose(f); snprintf(errbuf, sizeof(errbuf), "failed to read module: %s", full_path); cg_error(cg, 0, errbuf); return 1; }
    source[size] = '\0';
    fclose(f);

    /* Parse */
    Parser parser;
    parser_init(&parser, source);
    ASTNode *prog = parser_parse_program(&parser);
    if (parser.had_error) {
        free(source);
        cg_error(cg, 0, "module parse errors");
        return 1;
    }

    /* Verify top-level is a single PEICE_DECL. parser_parse_program() wraps
       a whole file's statements in NODE_PROGRAM (not NODE_BLOCK) - both
       use the same `as.block.statements` layout, so accept either. */
    if ((prog->type != NODE_BLOCK && prog->type != NODE_PROGRAM) ||
        prog->as.block.statements.count != 1 ||
        prog->as.block.statements.items[0]->type != NODE_PEICE_DECL) {
        free(source);
        ast_free(prog);
        cg_error(cg, 0, "module must contain exactly one PEICE declaration");
        return 1;
    }
    ASTNode *peice = prog->as.block.statements.items[0];
    const char *peice_name = peice->as.peice_decl.name;

    /* Check module already loaded */
    CgModule *mod = cg_module_find(cg, peice_name);
    if (mod) {
        if (mod->loading) {
            free(source);
            ast_free(prog);
            snprintf(errbuf, sizeof(errbuf), "circular module dependency: %s", peice_name);
            cg_error(cg, 0, errbuf);
            return 1;
        }
        if (mod->loaded) {
            free(source);
            ast_free(prog);
            return 0; /* already loaded */
        }
    } else {
        mod = cg_module_ensure(cg, peice_name);
    }
    mod->loading = 1;
    mod->peice_ast = peice;  /* store for later compilation */

    /* Recursively load nested BRINGs in the PEICE body */
    ASTNode *body = peice->as.peice_decl.body;
    for (int i = 0; i < body->as.block.statements.count; i++) {
        ASTNode *stmt = body->as.block.statements.items[i];
        if (stmt->type == NODE_BRING_STMT) {
            const char *mod_dir = full_path;
            char *slash = strrchr(full_path, '/');
            if (slash) {
                mod_dir = full_path;
                *slash = '\0';
                if (cg_module_load(cg, stmt->as.bring_stmt.path, mod_dir)) {
                    *slash = '/';
                    mod->loading = 0;
                    free(source);
                    ast_free(prog);
                    return 1;
                }
                *slash = '/';
            } else {
                if (cg_module_load(cg, stmt->as.bring_stmt.path, base_dir)) {
                    mod->loading = 0;
                    free(source);
                    ast_free(prog);
                    return 1;
                }
            }
        }
    }

    /* Collect SHIPped members from the PEICE */
    for (int i = 0; i < body->as.block.statements.count; i++) {
        ASTNode *stmt = body->as.block.statements.items[i];
        if (stmt->type == NODE_SHIP_STMT) {
            const char *ship_name = stmt->as.ship_stmt.name;
            /* Check if it's a job or variable declaration */
            int is_job = 0;
            for (int j = 0; j < body->as.block.statements.count; j++) {
                ASTNode *other = body->as.block.statements.items[j];
                if (other->type == NODE_JOB_DECL && strcmp(other->as.job_decl.name, ship_name) == 0) {
                    is_job = 1;
                    break;
                }
                if ((other->type == NODE_VAR_DECL || other->type == NODE_HARD_DECL) &&
                    strcmp(other->as.hard_decl.name, ship_name) == 0) {
                    is_job = 0;
                    break;
                }
            }
            CgModMemberKind kind = is_job ? CG_MOD_MEMBER_JOB : CG_MOD_MEMBER_VAR;
            CgModMember *mm = cg_module_member_add(mod, ship_name, kind);
            mm->is_exported = 1;
        }
    }

    mod->loaded = 1;
    mod->loading = 0;
    free(source);
    /* Do NOT ast_free(prog) - we need the PEICE AST for compilation later */
    return 0;
}

static void cg_do_push(CG *cg, const char *flag_label, int end_label) {
    if (cg->do_depth == cg->do_cap) {
        cg->do_cap = cg->do_cap ? cg->do_cap * 2 : 4;
        cg->do_stack = realloc(cg->do_stack, sizeof(CDo) * cg->do_cap);
    }
    cg->do_stack[cg->do_depth].flag_label = flag_label;
    cg->do_stack[cg->do_depth].end_label = end_label;
    cg->do_depth++;
}
static const CDo *cg_do_top(const CG *cg) {
    return cg->do_depth > 0 ? &cg->do_stack[cg->do_depth - 1] : NULL;
}
static void cg_do_pop(CG *cg) { if (cg->do_depth > 0) cg->do_depth--; }

/* ---- global VMA slot lookup/creation ---- */
static const char *global_label_for_addr(CG *cg, const char *addr) {
    for (int i = 0; i < cg->gcount; i++) if (strcmp(cg->globals[i].addr, addr) == 0) return cg->globals[i].label;
    if (cg->gcount == cg->gcap) { cg->gcap = cg->gcap ? cg->gcap * 2 : 16; cg->globals = realloc(cg->globals, sizeof(GVma) * cg->gcap); }
    GVma *g = &cg->globals[cg->gcount++];
    snprintf(g->addr, sizeof(g->addr), "%s", addr);
    snprintf(g->label, sizeof(g->label), "vma_%s", addr);
    return g->label;
}

/* ---- operand: where an expression's value currently lives ---- */
typedef struct {
    char text[48];  /* e.g. "-48(%rbp)" or "vma_A1(%rip)" or "iter3(%rip)" */
    int is_temp;    /* 1 => a stack slot owned by the expression evaluator */
} Opnd;

static Opnd alloc_temp(CG *cg) {
    Opnd o;
    o.is_temp = 1;
    long off = (long)(cg->depth + 1) * SLOT_SIZE;
    snprintf(o.text, sizeof(o.text), "-%ld(%%rbp)", off);
    cg->depth++;
    if (cg->depth > cg->max_depth) cg->max_depth = cg->depth;
    return o;
}
static void free_temp(CG *cg, Opnd *o) {
    if (o->is_temp) cg->depth--;
}

static Opnd global_opnd(const char *label) {
    Opnd o; o.is_temp = 0; snprintf(o.text, sizeof(o.text), "%s(%%rip)", label); return o;
}

/* ---- emitting calls: dst/a/b are Opnd.text (memory operands), addr-of
   is taken with lea; `line` args are plain immediates. Args 1-4 travel in
   the ABI argument registers; any 5th+ are passed on the stack (pushed in
   reverse for SysV, stored into the reserved shadow area for Win64). ---- */
static void emit_lea(CG *cg, const char *opnd, const char *reg) {
    fprintf(cg->body, "    leaq %s, %s\n", opnd, reg);
}
static void call0(CG *cg, const char *fn) { fprintf(cg->body, "    call %s\n", fn); }

static void callN(CG *cg, const char *fn, int n, const char *opnds[], int is_imm[]) {
    for (int i = 0; i < n && i < 4; i++) {
        if (is_imm[i]) fprintf(cg->body, "    movq $%s, %s\n", opnds[i], cg->abi.argreg[i]);
        else emit_lea(cg, opnds[i], cg->abi.argreg[i]);
    }
    if (n > 4) {
        if (cg->target == CG_TARGET_LINUX) {
            for (int i = n - 1; i >= 4; i--) {
                if (is_imm[i]) fprintf(cg->body, "    pushq $%s\n", opnds[i]);
                else fprintf(cg->body, "    pushq %s\n", opnds[i]);
            }
        } else { /* win64: store into the already-reserved shadow + spill area */
            for (int i = 4; i < n; i++) {
                if (is_imm[i]) fprintf(cg->body, "    movq $%s, %d(%%rsp)\n", opnds[i], 32 + 8 * (i - 4));
                else fprintf(cg->body, "    leaq %s, %%r11\n    movq %%r11, %d(%%rsp)\n", opnds[i], 32 + 8 * (i - 4));
            }
        }
    }
    call0(cg, fn);
    if (n > 4 && cg->target == CG_TARGET_LINUX)
        fprintf(cg->body, "    addq $%d, %%rsp\n", (n - 4) * 8);
    if (n > 4 && cg->target == CG_TARGET_WINDOWS && (n - 4) > cg->max_stack_args)
        cg->max_stack_args = n - 4;
}

static void call1(CG *cg, const char *fn, const char *a0) { const char *o[1]={a0}; int im[1]={0}; callN(cg,fn,1,o,im); }
static void call2(CG *cg, const char *fn, const char *a0, const char *a1) { const char *o[2]={a0,a1}; int im[2]={0,0}; callN(cg,fn,2,o,im); }
static void call3(CG *cg, const char *fn, const char *a0, const char *a1, const char *a2) { const char *o[3]={a0,a1,a2}; int im[3]={0,0,0}; callN(cg,fn,3,o,im); }
static void call3_imm2(CG *cg, const char *fn, const char *a0, const char *a1, long line) {
    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%ld", line);
    const char *o[3]={a0,a1,linebuf}; int im[3]={0,0,1}; callN(cg,fn,3,o,im);
}
static void call4_imm(CG *cg, const char *fn, const char *a0, const char *a1, const char *a2, long line) {
    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%ld", line);
    const char *o[4]={a0,a1,a2,linebuf}; int im[4]={0,0,0,1}; callN(cg,fn,4,o,im);
}
static void call5_imm(CG *cg, const char *fn, const char *a0, const char *a1, const char *a2, const char *a3, long line) {
    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%ld", line);
    const char *o[5]={a0,a1,a2,a3,linebuf}; int im[5]={0,0,0,0,1}; callN(cg,fn,5,o,im);
}

static void free_if_temp(CG *cg, Opnd *o) {
    if (o->is_temp) { call1(cg, "vo_free", o->text); free_temp(cg, o); }
}

static int new_label(CG *cg) { return cg->label_id++; }

/* forward decls */
static Opnd emit_expr(CG *cg, ASTNode *node);
static Opnd emit_command(CG *cg, ASTNode *node);
static void emit_stmt(CG *cg, ASTNode *node);
static void emit_block(CG *cg, ASTNode *block);
static void emit_give_stmt(CG *cg, ASTNode *node);
static void emit_demand_stmt(CG *cg, ASTNode *node);
static void emit_serve_stmt(CG *cg, ASTNode *node);
static void emit_do_stmt(CG *cg, ASTNode *node);

/* ---- string literal constants ---- */
static const char *intern_string(CG *cg, const char *s) {
    static char label[40];
    int id = cg->str_id++;
    snprintf(label, sizeof(label), "str_%d", id);
    fprintf(cg->rodata, "%s:\n    .string \"", label);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', cg->rodata);
        if (*p == '\n') { fprintf(cg->rodata, "\\n"); continue; }
        fputc(*p, cg->rodata);
    }
    fprintf(cg->rodata, "\"\n");
    return label; /* pointer to the static buffer above - copied out by caller immediately */
}

/* Resolve a call target to its CJob (mirrors emit_call_expr's resolution
   rules, including module-qualified calls and same-module sibling lookups).
   Returns NULL and reports the compile error on failure. */
static CJob *cg_resolve_job(CG *cg, ASTNode *callee, int line) {
    if (callee->type == NODE_MODULE_REF) {
        const char *mod_name = callee->as.module_ref.module;
        const char *member_name = callee->as.module_ref.member;
        CgModule *mod = cg_module_find(cg, mod_name);
        if (!mod) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "unknown module '%s'", mod_name); cg_error(cg, line, errbuf); return NULL; }
        CgModMember *mm = cg_module_member_find(mod, member_name);
        if (!mm) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' has no member '%s'", mod_name, member_name); cg_error(cg, line, errbuf); return NULL; }
        if (!mm->is_exported) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' member '%s' is not exported", mod_name, member_name); cg_error(cg, line, errbuf); return NULL; }
        if (mm->kind != CG_MOD_MEMBER_JOB) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' member '%s' is not a job", mod_name, member_name); cg_error(cg, line, errbuf); return NULL; }
        if (!mm->job_internal) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' job '%s' not found (internal error)", mod_name, member_name); cg_error(cg, line, errbuf); return NULL; }
        CJob *j = cg_job_lookup(cg, mm->job_internal);
        if (!j) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "internal job '%s' not found", mm->job_internal); cg_error(cg, line, errbuf); return NULL; }
        return j;
    }
    if (callee->type == NODE_IDENTIFIER) {
        const char *fname = callee->as.identifier.name;
        CJob *job = NULL;
        if (cg->cur_job_module) {
            char qualified[128];
            snprintf(qualified, sizeof(qualified), "%s#%s", cg->cur_job_module, fname);
            job = cg_job_lookup(cg, qualified);
        }
        if (!job) job = cg_job_lookup(cg, fname);
        if (!job) { cg_error(cg, line, "call to unknown job (analyzer should have caught this)"); return NULL; }
        return job;
    }
    cg_error(cg, line, "only direct function calls are supported");
    return NULL;
}

/* ---- JOB call expression ------------------------------------------------ */
/* Arguments are copied into the callee job's dedicated global parameter
   slots (mirrors the interpreter's VMA-backed param binding), then the job
   function runs and leaves its result in its global return slot. */
static Opnd emit_call_expr(CG *cg, ASTNode *node) {
    ASTNode *callee = node->as.call.callee;
    CJob *job = cg_resolve_job(cg, callee, node->line);

    if (!job) {
        Opnd bad = alloc_temp(cg);
        call1(cg, "vo_set_emp", bad.text);
        return bad;
    }

    int nargs = node->as.call.args.count;
    if (nargs != job->param_count) {
        cg_error(cg, node->line, "wrong number of arguments (analyzer should have caught this)");
        Opnd bad = alloc_temp(cg);
        call1(cg, "vo_set_emp", bad.text);
        return bad;
    }

    Opnd temps[16];
    for (int i = 0; i < nargs; i++) temps[i] = emit_expr(cg, node->as.call.args.items[i]);

    for (int i = 0; i < nargs; i++) {
        char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", job->param_labels[i]);
        call2(cg, "vo_assign", rip, temps[i].text);
        free_if_temp(cg, &temps[i]);
    }

    char retrip[48]; snprintf(retrip, sizeof(retrip), "%s(%%rip)", job->ret_label);
    call1(cg, "vo_set_emp", retrip);
    fprintf(cg->body, "    call vo_job_%d\n", job->id);

    Opnd out = alloc_temp(cg);
    call1(cg, "vo_set_emp", out.text);
    call2(cg, "vo_assign", out.text, retrip);
    return out;
}

static void call2_imm(CG *cg, const char *fn, const char *a0, long line) {
    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%ld", line);
    const char *o[2]={a0,linebuf}; int im[2]={0,1}; callN(cg,fn,2,o,im);
}

/* ---- builtin commands (v1.4) ---- */
/* A mutating command's FIRST operand is an lvalue (its collection/file/
   task storage is written in place). Identifiers and bare VMAs resolve to
   their persistent slot; INDEX targets and arbitrary expressions are
   reported as native-compile limitations (the tree-walking interpreter
   still accepts them). */
static Opnd emit_lvalue_target(CG *cg, ASTNode *op, int line) {
    if (op->type == NODE_IDENTIFIER) {
        CSym *sym = scope_resolve(cg->scope, op->as.identifier.name);
        if (!sym) { cg_error(cg, line, "internal: unresolved identifier (analyzer should have caught this)"); }
        else return global_opnd(sym->is_loop_var ? sym->loop_label : global_label_for_addr(cg, sym->vma));
    }
    if (op->type == NODE_VMA_REF)
        return global_opnd(global_label_for_addr(cg, op->as.vma_ref.name));
    cg_error(cg, line, "indexed / non-variable command targets are not supported by the native compiler (voc) - use a named variable or VMA");
    Opnd bad = alloc_temp(cg);
    call1(cg, "vo_set_emp", bad.text);
    return bad;
}

/* HOLD/CLAIM/HALT take a task lvalue when possible (so state like "run
   once" persists in the owning variable); otherwise a plain expression. */
static Opnd emit_task_operand(CG *cg, ASTNode *op, int line) {
    if (op->type == NODE_IDENTIFIER) {
        CSym *sym = scope_resolve(cg->scope, op->as.identifier.name);
        if (sym) return global_opnd(sym->is_loop_var ? sym->loop_label : global_label_for_addr(cg, sym->vma));
    }
    if (op->type == NODE_VMA_REF)
        return global_opnd(global_label_for_addr(cg, op->as.vma_ref.name));
    if (op->type == NODE_INDEX) {
        cg_error(cg, line, "indexed task operands are not supported by the native compiler (voc)");
        Opnd bad = alloc_temp(cg);
        call1(cg, "vo_set_emp", bad.text);
        return bad;
    }
    return emit_expr(cg, op);
}

static Opnd emit_command(CG *cg, ASTNode *node) {
    CommandKind kind = node->as.command.kind;
    NodeList *a = &node->as.command.args;
    int line = node->line;
    int n = a->count;

    Opnd out = alloc_temp(cg);
    call1(cg, "vo_set_emp", out.text);

    switch (kind) {
    case CMD_ATTACH: {
        Opnd coll = emit_lvalue_target(cg, a->items[0], line);
        Opnd item = emit_expr(cg, a->items[1]);
        call3_imm2(cg, "vo_cmd_attach", coll.text, item.text, line);
        free_if_temp(cg, &item);
        break;
    }
    case CMD_PLACE: {
        Opnd coll = emit_lvalue_target(cg, a->items[0], line);
        Opnd idx = emit_expr(cg, a->items[1]);
        Opnd item = emit_expr(cg, a->items[2]);
        call4_imm(cg, "vo_cmd_place", coll.text, idx.text, item.text, line);
        free_if_temp(cg, &item);
        free_if_temp(cg, &idx);
        break;
    }
    case CMD_ERASE:
        if (n == 2) {
            Opnd coll = emit_lvalue_target(cg, a->items[0], line);
            Opnd idx = emit_expr(cg, a->items[1]);
            call3_imm2(cg, "vo_cmd_erase_coll", coll.text, idx.text, line);
            free_if_temp(cg, &idx);
        } else {
            Opnd p = emit_expr(cg, a->items[0]);
            call2_imm(cg, "vo_cmd_erase_file", p.text, line);
            free_if_temp(cg, &p);
        }
        break;
    case CMD_COUNT: {
        Opnd v = emit_expr(cg, a->items[0]);
        call3_imm2(cg, "vo_cmd_count", out.text, v.text, line);
        free_if_temp(cg, &v);
        break;
    }
    case CMD_TAKE: {
        Opnd v = emit_expr(cg, a->items[0]);
        call3_imm2(cg, "vo_cmd_take", out.text, v.text, line);
        free_if_temp(cg, &v);
        break;
    }
    case CMD_SEEK: {
        Opnd hay = emit_expr(cg, a->items[0]);
        Opnd ndl = emit_expr(cg, a->items[1]);
        call4_imm(cg, "vo_cmd_seek", out.text, hay.text, ndl.text, line);
        free_if_temp(cg, &ndl);
        free_if_temp(cg, &hay);
        break;
    }
    case CMD_HAS:
        if (n == 1) {
            Opnd p = emit_expr(cg, a->items[0]);
            call3_imm2(cg, "vo_cmd_has_path", out.text, p.text, line);
            free_if_temp(cg, &p);
        } else {
            Opnd hay = emit_expr(cg, a->items[0]);
            Opnd ndl = emit_expr(cg, a->items[1]);
            call4_imm(cg, "vo_cmd_has", out.text, hay.text, ndl.text, line);
            free_if_temp(cg, &ndl);
            free_if_temp(cg, &hay);
        }
        break;
    case CMD_BIND: {
        Opnd coll = emit_expr(cg, a->items[0]);
        Opnd sep = emit_expr(cg, a->items[1]);
        call4_imm(cg, "vo_cmd_bind", out.text, coll.text, sep.text, line);
        free_if_temp(cg, &sep);
        free_if_temp(cg, &coll);
        break;
    }
    case CMD_SEVER: {
        Opnd tex = emit_expr(cg, a->items[0]);
        Opnd sep = emit_expr(cg, a->items[1]);
        call4_imm(cg, "vo_cmd_sever", out.text, tex.text, sep.text, line);
        free_if_temp(cg, &sep);
        free_if_temp(cg, &tex);
        break;
    }
    case CMD_CUT: {
        Opnd v = emit_expr(cg, a->items[0]);
        call3_imm2(cg, "vo_cmd_cut", out.text, v.text, line);
        free_if_temp(cg, &v);
        break;
    }
    case CMD_RAISE:
    case CMD_LOWER: {
        Opnd v = emit_expr(cg, a->items[0]);
        call4_imm(cg, "vo_cmd_case", out.text, v.text, kind == CMD_RAISE ? "1" : "0", line);
        free_if_temp(cg, &v);
        break;
    }
    case CMD_UNSEAL: {
        Opnd path = emit_expr(cg, a->items[0]);
        Opnd mode;
        if (n == 2) mode = emit_expr(cg, a->items[1]);
        else { mode = alloc_temp(cg); call1(cg, "vo_set_emp", mode.text); }
        char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", line);
        const char *o[5] = { out.text, path.text, mode.text, n == 2 ? "1" : "0", linebuf };
        int im[5] = { 0, 0, 0, 1, 1 };
        callN(cg, "vo_cmd_unseal", 5, o, im);
        free_if_temp(cg, &mode);
        free_if_temp(cg, &path);
        break;
    }
    case CMD_SEAL: {
        Opnd f = emit_expr(cg, a->items[0]);
        call2_imm(cg, "vo_cmd_seal", f.text, line);
        free_if_temp(cg, &f);
        break;
    }
    case CMD_DRAW: {
        Opnd f = emit_expr(cg, a->items[0]);
        Opnd cnt;
        if (n == 2) cnt = emit_expr(cg, a->items[1]);
        else { cnt = alloc_temp(cg); call1(cg, "vo_set_emp", cnt.text); }
        char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", line);
        const char *o[5] = { out.text, f.text, cnt.text, n == 2 ? "1" : "0", linebuf };
        int im[5] = { 0, 0, 0, 1, 1 };
        callN(cg, "vo_cmd_draw", 5, o, im);
        free_if_temp(cg, &cnt);
        free_if_temp(cg, &f);
        break;
    }
    case CMD_PUT: {
        Opnd f = emit_expr(cg, a->items[0]);
        Opnd t = emit_expr(cg, a->items[1]);
        call3_imm2(cg, "vo_cmd_put", f.text, t.text, line);
        free_if_temp(cg, &t);
        free_if_temp(cg, &f);
        break;
    }
    case CMD_MOVE: {
        Opnd f = emit_expr(cg, a->items[0]);
        Opnd pos = emit_expr(cg, a->items[1]);
        call3_imm2(cg, "vo_cmd_move", f.text, pos.text, line);
        free_if_temp(cg, &pos);
        free_if_temp(cg, &f);
        break;
    }
    case CMD_MAKE: {
        Opnd p1 = emit_expr(cg, a->items[0]);
        call2_imm(cg, "vo_cmd_make", p1.text, line);
        free_if_temp(cg, &p1);
        break;
    }
    case CMD_RECALL:
    case CMD_CLONE:
    case CMD_DELIVER: {
        const char *fn = kind == CMD_RECALL ? "vo_cmd_recall"
                        : kind == CMD_CLONE ? "vo_cmd_clone" : "vo_cmd_deliver";
        Opnd p1 = emit_expr(cg, a->items[0]);
        Opnd p2 = emit_expr(cg, a->items[1]);
        call3_imm2(cg, fn, p1.text, p2.text, line);
        free_if_temp(cg, &p2);
        free_if_temp(cg, &p1);
        break;
    }
    case CMD_SPAWN: {
        ASTNode *call = a->items[0];
        if (call->type != NODE_CALL) { cg_error(cg, line, "SPAWN expects a job call"); break; }
        CJob *job = cg_resolve_job(cg, call->as.call.callee, line);
        if (!job) break;
        if (call->as.call.args.count != job->param_count) { cg_error(cg, line, "SPAWN: wrong number of arguments"); break; }
        char jidx[16], argc[16];
        snprintf(jidx, sizeof(jidx), "%d", job->tab_index);
        snprintf(argc, sizeof(argc), "%d", job->param_count);
        char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", line);
        const char *o[4] = { out.text, jidx, argc, linebuf };
        int im[4] = { 0, 1, 1, 1 };
        callN(cg, "vo_cmd_spawn", 4, o, im);
        for (int i = 0; i < job->param_count; i++) {
            Opnd arg = emit_expr(cg, call->as.call.args.items[i]);
            char ibuf[8]; snprintf(ibuf, sizeof(ibuf), "%d", i);
            const char *so[4] = { out.text, ibuf, arg.text, linebuf };
            int sim[4] = { 0, 1, 0, 1 };
            callN(cg, "vo_task_set_arg", 4, so, sim);
            free_if_temp(cg, &arg);
        }
        break;
    }
    case CMD_HOLD: {
        Opnd op = emit_task_operand(cg, a->items[0], line);
        call2_imm(cg, "vo_cmd_hold", op.text, line);
        free_if_temp(cg, &op);
        break;
    }
    case CMD_CLAIM: {
        Opnd op = emit_task_operand(cg, a->items[0], line);
        call3_imm2(cg, "vo_cmd_claim", out.text, op.text, line);
        free_if_temp(cg, &op);
        break;
    }
    case CMD_HALT: {
        Opnd op = emit_task_operand(cg, a->items[0], line);
        call2_imm(cg, "vo_cmd_halt", op.text, line);
        free_if_temp(cg, &op);
        break;
    }
    case CMD_SEIZE:
    case CMD_RELEASE:
    case CMD_ALIGN: {
        Opnd lk = emit_expr(cg, a->items[0]);
        const char *fn = kind == CMD_SEIZE ? "vo_cmd_seize"
                      : kind == CMD_RELEASE ? "vo_cmd_release" : "vo_cmd_align";
        call2_imm(cg, fn, lk.text, line);
        free_if_temp(cg, &lk);
        break;
    }
    default:
        cg_error(cg, line, "event commands (ARM/DISARM/FIRE/RANK/KILL/SCREEN/LINK) are not supported by the "
                          "native compiler (voc) - the tree-walking interpreter handles them");
        break;
    }

    return out;
}

/* =========================================================================
 * Expressions
 * ========================================================================= */
static Opnd emit_expr(CG *cg, ASTNode *node) {
    Opnd out;
    switch (node->type) {
        case NODE_NUM_LITERAL: {
            out = alloc_temp(cg);
            char imm[32]; snprintf(imm, sizeof(imm), "%ld", node->as.num_lit.value);
            const char *o[2] = { out.text, imm }; int im[2] = { 0, 1 };
            callN(cg, "vo_set_num", 2, o, im);
            return out;
        }
        case NODE_BOOL_LITERAL: {
            out = alloc_temp(cg);
            const char *o[2] = { out.text, node->as.bool_lit.value ? "1" : "0" }; int im[2] = { 0, 1 };
            callN(cg, "vo_set_yn", 2, o, im);
            return out;
        }
        case NODE_EMP_LITERAL:
            out = alloc_temp(cg);
            call1(cg, "vo_set_emp", out.text);
            return out;
        case NODE_DEC_LITERAL: {
            /* stash the double as raw bits so we can move it with a plain
               integer immediate, then call vo_set_num-style setter via a
               tiny helper that reinterprets - simplest: encode literal as
               a rodata double and load through memory via a helper call. */
            out = alloc_temp(cg);
            static char lbl[40]; snprintf(lbl, sizeof(lbl), "dbl_%d", cg->str_id++);
            fprintf(cg->rodata, "    .align 8\n%s:\n    .double %.17g\n", lbl, node->as.dec_lit.value);
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
            call2(cg, "vo_set_dec_from_ptr", out.text, rip);
            return out;
        }
        case NODE_TEX_LITERAL: {
            out = alloc_temp(cg);
            const char *lbl = intern_string(cg, node->as.tex_lit.value);
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
            call2(cg, "vo_set_tex", out.text, rip);
            return out;
        }
        case NODE_CALL:
            return emit_call_expr(cg, node);
        case NODE_IDENTIFIER: {
            CSym *sym = scope_resolve(cg->scope, node->as.identifier.name);
            if (!sym) { cg_error(cg, node->line, "internal: unresolved identifier (analyzer should have caught this)"); out = alloc_temp(cg); return out; }
            return global_opnd(sym->is_loop_var ? sym->loop_label : global_label_for_addr(cg, sym->vma));
        }
        case NODE_VMA_REF:
            return global_opnd(global_label_for_addr(cg, node->as.vma_ref.name));
        case NODE_LOAD:
            return emit_expr(cg, node->as.load.vma);
        case NODE_MODULE_REF: {
            /* Module member value reference: math.NAME */
            const char *mod_name = node->as.module_ref.module;
            const char *member_name = node->as.module_ref.member;
            CgModule *mod = cg_module_find(cg, mod_name);
            if (!mod) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "unknown module '%s'", mod_name); cg_error(cg, node->line, errbuf); out = alloc_temp(cg); return out; }
            CgModMember *mm = cg_module_member_find(mod, member_name);
            if (!mm) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' has no member '%s'", mod_name, member_name); cg_error(cg, node->line, errbuf); out = alloc_temp(cg); return out; }
            if (!mm->is_exported) { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' member '%s' is not exported", mod_name, member_name); cg_error(cg, node->line, errbuf); out = alloc_temp(cg); return out; }
            if (mm->kind == CG_MOD_MEMBER_VAR) {
                if (mm->var_label[0] == '\0') { char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module member '%s.%s' variable not initialized", mod_name, member_name); cg_error(cg, node->line, errbuf); out = alloc_temp(cg); return out; }
                return global_opnd(mm->var_label);
            } else {
                char errbuf[256]; snprintf(errbuf, sizeof(errbuf), "module '%s' member '%s' is a job, not a value", mod_name, member_name); cg_error(cg, node->line, errbuf);
                out = alloc_temp(cg);
                return out;
            }
        }
        case NODE_UNARY: {
            Opnd a = emit_expr(cg, node->as.unary.operand);
            /* `a` may be a variable's own persistent storage (not a scratch
               temp) - never write the result in place over it, or reading
               that identifier again later would see the wrong value. */
            Opnd dst;
            if (a.is_temp) dst = a;
            else { dst = alloc_temp(cg); call2(cg, "vo_copy", dst.text, a.text); }
            switch (node->as.unary.op) {
                case TOKEN_MINUS: call3_imm2(cg, "vo_neg", dst.text, dst.text, node->line); break;
                case TOKEN_NOT:   call2(cg, "vo_lnot", dst.text, dst.text); break;
                default: cg_error(cg, node->line, "unsupported unary operator"); break;
            }
            return dst;
        }
        case NODE_BINARY: {
            Opnd l = emit_expr(cg, node->as.binary.left);
            Opnd r = emit_expr(cg, node->as.binary.right);
            /* Same rule as above: only reuse `l` as the output slot when it
               is already a scratch temp we own outright. */
            Opnd dst;
            if (l.is_temp) dst = l;
            else { dst = alloc_temp(cg); call2(cg, "vo_copy", dst.text, l.text); }
            const char *fn = NULL; int needs_line = 1;
            switch (node->as.binary.op) {
                case TOKEN_PLUS: fn = "vo_add"; break;
                case TOKEN_MINUS: fn = "vo_sub"; break;
                case TOKEN_STAR: fn = "vo_mul"; break;
                case TOKEN_SLASH: fn = "vo_div"; break;
                case TOKEN_PERCENT: fn = "vo_mod"; break;
                case TOKEN_POWER: fn = "vo_pow"; break;
                case TOKEN_SHL: fn = "vo_shl"; break;
                case TOKEN_SHR: fn = "vo_shr"; break;
                case TOKEN_AMP: fn = "vo_band"; break;
                case TOKEN_CARET: fn = "vo_bxor"; break;
                case TOKEN_PIPE: fn = "vo_bor"; break;
                case TOKEN_LT: fn = "vo_lt"; break;
                case TOKEN_GT: fn = "vo_gt"; break;
                case TOKEN_LE: fn = "vo_le"; break;
                case TOKEN_GE: fn = "vo_ge"; break;
                case TOKEN_EQEQ: fn = "vo_eq"; needs_line = 0; break;
                case TOKEN_NEQ:  fn = "vo_neq"; needs_line = 0; break;
                case TOKEN_AND:  fn = "vo_land"; needs_line = 0; break;
                case TOKEN_OR:   fn = "vo_lor"; needs_line = 0; break;
                case TOKEN_XOR:  fn = "vo_lxor"; needs_line = 0; break;
                default: cg_error(cg, node->line, "unsupported binary operator"); fn = "vo_add"; break;
            }
            if (needs_line) call4_imm(cg, fn, dst.text, l.text, r.text, node->line);
            else call3(cg, fn, dst.text, l.text, r.text);
            free_if_temp(cg, &r);
            return dst;
        }
        case NODE_LENGTH_CALL: {
            Opnd a = emit_expr(cg, node->as.length_call.arg);
            out = alloc_temp(cg);
            call3_imm2(cg, "vo_length", out.text, a.text, node->line);
            free_if_temp(cg, &a);
            return out;
        }
        case NODE_ARRAY_LITERAL: {
            out = alloc_temp(cg);
            call1(cg, "vo_coll_new", out.text);
            for (int i = 0; i < node->as.array_lit.elements.count; i++) {
                Opnd e = emit_expr(cg, node->as.array_lit.elements.items[i]);
                call3_imm2(cg, "vo_coll_push", out.text, e.text, node->line);
                free_if_temp(cg, &e);
            }
            return out;
        }
        case NODE_INDEX: {
            Opnd arr = emit_expr(cg, node->as.index_expr.array);
            Opnd idx = emit_expr(cg, node->as.index_expr.index);
            out = alloc_temp(cg);
            char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", node->line);
            const char *o[4] = { out.text, arr.text, idx.text, linebuf }; int im[4] = {0,0,0,1};
            callN(cg, "vo_index_get", 4, o, im);
            free_if_temp(cg, &idx);
            free_if_temp(cg, &arr);
            return out;
        }
        case NODE_SLICE: {
            Opnd arr = emit_expr(cg, node->as.slice_expr.array);
            Opnd start = emit_expr(cg, node->as.slice_expr.start);
            Opnd end = emit_expr(cg, node->as.slice_expr.end);
            out = alloc_temp(cg);
            call5_imm(cg, "vo_slice", out.text, arr.text, start.text, end.text, node->line);
            free_if_temp(cg, &end);
            free_if_temp(cg, &start);
            free_if_temp(cg, &arr);
            return out;
        }
        case NODE_INTERP_LITERAL: {
            out = alloc_temp(cg);
            const char *lbl = intern_string(cg, node->as.interp_lit.text ? node->as.interp_lit.text : "");
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
            call2(cg, "vo_set_tex", out.text, rip);
            return out;
        }
        case NODE_TEX_INTERP: {
            out = alloc_temp(cg);
            {
                const char *lbl = intern_string(cg, "");
                char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
                call2(cg, "vo_set_tex", out.text, rip);
            }
            for (int i = 0; i < node->as.tex_interp.parts.count; i++) {
                ASTNode *part = node->as.tex_interp.parts.items[i];
                if (part->type == NODE_INTERP_LITERAL) {
                    const char *lbl = intern_string(cg, part->as.interp_lit.text ? part->as.interp_lit.text : "");
                    char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
                    call2(cg, "vo_tex_interp_append_cstr", out.text, rip);
                } else {
                    Opnd v = emit_expr(cg, part);
                    call2(cg, "vo_tex_interp_append_val", out.text, v.text);
                    free_if_temp(cg, &v);
                }
            }
            return out;
        }
        case NODE_COMMAND:
            return emit_command(cg, node);
        case NODE_ASSIGN: {
            Opnd rhs = emit_expr(cg, node->as.assign.value);
            ASTNode *target = node->as.assign.target;
            if (target->type == NODE_INDEX) {
                Opnd arr = emit_expr(cg, target->as.index_expr.array);
                Opnd idx = emit_expr(cg, target->as.index_expr.index);
                if (node->as.assign.op == TOKEN_ASSIGN) {
                    call4_imm(cg, "vo_index_set", arr.text, idx.text, rhs.text, node->line);
                } else {
                    Opnd cur = alloc_temp(cg);
                    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", node->line);
                    const char *go[4] = { cur.text, arr.text, idx.text, linebuf }; int gim[4] = {0,0,0,1};
                    callN(cg, "vo_index_get", 4, go, gim);
                    const char *fn = node->as.assign.op == TOKEN_PLUS_ASSIGN ? "vo_plus_assign"
                                     : node->as.assign.op == TOKEN_MINUS_ASSIGN ? "vo_sub"
                                     : node->as.assign.op == TOKEN_STAR_ASSIGN ? "vo_mul"
                                     : node->as.assign.op == TOKEN_SLASH_ASSIGN ? "vo_div"
                                     : node->as.assign.op == TOKEN_PERCENT_ASSIGN ? "vo_mod" : "vo_add";
                    call4_imm(cg, fn, cur.text, cur.text, rhs.text, node->line);
                    call4_imm(cg, "vo_index_set", arr.text, idx.text, cur.text, node->line);
                    free_if_temp(cg, &cur);
                }
                free_if_temp(cg, &idx);
                free_if_temp(cg, &arr);
                free_if_temp(cg, &rhs);
                return global_opnd("__unused__"); /* assignment-as-expression result unused for index targets in v1 */
            }
            Opnd dst = emit_expr(cg, target); /* identifier or bare VMA - a persistent global operand */
            if (node->as.assign.op == TOKEN_ASSIGN) {
                call2(cg, "vo_assign", dst.text, rhs.text);
            } else if (node->as.assign.op == TOKEN_PLUS_ASSIGN) {
                call4_imm(cg, "vo_plus_assign", dst.text, dst.text, rhs.text, node->line);
            } else {
                const char *fn = node->as.assign.op == TOKEN_MINUS_ASSIGN ? "vo_sub"
                                 : node->as.assign.op == TOKEN_STAR_ASSIGN ? "vo_mul"
                                 : node->as.assign.op == TOKEN_SLASH_ASSIGN ? "vo_div"
                                 : node->as.assign.op == TOKEN_PERCENT_ASSIGN ? "vo_mod" : "vo_add";
                call4_imm(cg, fn, dst.text, dst.text, rhs.text, node->line);
            }
            free_if_temp(cg, &rhs);
            return dst;
        }
        default:
            cg_error(cg, node->line, "this expression form is not yet supported by the native compiler");
            out = alloc_temp(cg);
            call1(cg, "vo_set_null", out.text);
            return out;
    }
}

/* =========================================================================
 * Statements
 * ========================================================================= */
static void declare_var(CG *cg, ASTNode *node) {
    int is_aliasing = node->as.var_decl.init && node->as.var_decl.init->type == NODE_VMA_REF;
    const char *name = node->as.var_decl.name;

    if (scope_find_local(cg->scope, name)) { cg_error(cg, node->line, "redeclaration in this scope"); return; }

    CVmaSlot *slot;
    if (is_aliasing) {
        const char *addr = node->as.var_decl.init->as.vma_ref.name;
        slot = vma_alloc_specific(&cg->vmas, addr);
        if (!slot) { cg_error(cg, node->line, "aliasing conflict: address already allocated"); return; }
    } else {
        slot = vma_alloc_next(&cg->vmas);
    }
    const char *label = global_label_for_addr(cg, slot->address);

    CSym *sym = scope_declare(cg->scope, name);
    snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
    sym->autocleans = cg->autoclean_on;

    if (is_aliasing) {
        /* bind only - reset to the type's default value, don't read A_addr's
           current contents (matches spec Sec 5 / the tree-walking runtime) */
        char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", label);
        switch (node->as.var_decl.var_type) {
            case TOKEN_TYPE_NUM: { const char *o[2]={rip,"0"}; int im[2]={0,1}; callN(cg,"vo_set_num",2,o,im); break; }
            case TOKEN_TYPE_DEC: {
                static char lbl[40]; snprintf(lbl, sizeof(lbl), "dbl_%d", cg->str_id++);
                fprintf(cg->rodata, "    .align 8\n%s:\n    .double 0.0\n", lbl);
                char rip2[48]; snprintf(rip2, sizeof(rip2), "%s(%%rip)", lbl);
                call2(cg, "vo_set_dec_from_ptr", rip, rip2);
                break;
            }
            case TOKEN_TYPE_TEX: { const char *lbl = intern_string(cg, ""); char rip2[48]; snprintf(rip2,sizeof(rip2),"%s(%%rip)",lbl); call2(cg,"vo_set_tex",rip,rip2); break; }
            case TOKEN_TYPE_YN:  { const char *o[2]={rip,"0"}; int im[2]={0,1}; callN(cg,"vo_set_yn",2,o,im); break; }
            case TOKEN_TYPE_COLL: call1(cg, "vo_coll_new", rip); break;
            default: call1(cg, "vo_set_null", rip); break;
        }
    } else {
        Opnd init = emit_expr(cg, node->as.var_decl.init);
        char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", label);
        call2(cg, "vo_assign", rip, init.text);
        free_if_temp(cg, &init);
    }
}

static void emit_stmt(CG *cg, ASTNode *node) {
    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_HARD_DECL:
            declare_var(cg, node);
            break;
        case NODE_EXPR_STMT: {
            Opnd v = emit_expr(cg, node->as.expr_stmt.expr);
            free_if_temp(cg, &v);
            break;
        }
        case NODE_SHOW_STMT: {
            int count = node->as.show_stmt.expr.count;
            for (int i = 0; i < count; i++) {
                Opnd v = emit_expr(cg, node->as.show_stmt.expr.items[i]);
                call1(cg, i == count - 1 ? "vo_show" : "vo_show_part", v.text);
                free_if_temp(cg, &v);
            }
            break;
        }
        case NODE_COMMAND: {
            Opnd v = emit_command(cg, node);
            free_if_temp(cg, &v);
            break;
        }
        case NODE_STORE_STMT: {
            Opnd v = emit_expr(cg, node->as.store_stmt.value);
            const char *label = global_label_for_addr(cg, node->as.store_stmt.target_vma);
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", label);
            call2(cg, "vo_assign", rip, v.text);
            free_if_temp(cg, &v);
            break;
        }
        case NODE_CLEAN_STMT: {
            const char *target = node->as.clean_stmt.target;
            const char *label;
            CSym *sym = scope_resolve(cg->scope, target);
            if (sym) label = sym->is_loop_var ? sym->loop_label : global_label_for_addr(cg, sym->vma);
            else label = global_label_for_addr(cg, target); /* raw VMA text */
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", label);
            call1(cg, "vo_free", rip);
            break;
        }
        case NODE_CLEANALL_STMT: {
            for (int i = 0; i < cg->gcount; i++) {
                char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", cg->globals[i].label);
                call1(cg, "vo_free", rip);
            }
            break;
        }
        case NODE_AUTOCLEAN_STMT:
            cg->autoclean_on = node->as.autoclean_stmt.on;
            break;
        case NODE_INC_DEC_STMT: {
            ASTNode *target = node->as.inc_dec.target;
            const char *fn = node->as.inc_dec.op == TOKEN_INCREMENT ? "vo_add" : "vo_sub";
            if (target->type == NODE_INDEX) {
                Opnd arr = emit_expr(cg, target->as.index_expr.array);
                Opnd idx = emit_expr(cg, target->as.index_expr.index);
                Opnd cur = alloc_temp(cg);
                char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", node->line);
                const char *go[4] = { cur.text, arr.text, idx.text, linebuf }; int gim[4] = {0,0,0,1};
                callN(cg, "vo_index_get", 4, go, gim);
                Opnd one = alloc_temp(cg);
                const char *oo[2] = { one.text, "1" }; int oim[2] = { 0, 1 };
                callN(cg, "vo_set_num", 2, oo, oim);
                call4_imm(cg, fn, cur.text, cur.text, one.text, node->line);
                call4_imm(cg, "vo_index_set", arr.text, idx.text, cur.text, node->line);
                free_temp(cg, &one);
                free_if_temp(cg, &cur);
                free_if_temp(cg, &idx);
                free_if_temp(cg, &arr);
            } else {
                Opnd dst = emit_expr(cg, target);
                Opnd one = alloc_temp(cg);
                const char *oo[2] = { one.text, "1" }; int oim[2] = { 0, 1 };
                callN(cg, "vo_set_num", 2, oo, oim);
                call4_imm(cg, fn, dst.text, dst.text, one.text, node->line);
                free_temp(cg, &one);
            }
            break;
        }
        case NODE_IF_STMT: {
            int end_lbl = new_label(cg);
            for (int i = 0; i < node->as.if_stmt.branches.count; i++) {
                ASTNode *br = node->as.if_stmt.branches.items[i];
                int next_lbl = new_label(cg);
                if (br->as.if_branch.condition) {
                    Opnd c = emit_expr(cg, br->as.if_branch.condition);
                    call1(cg, "vo_truthy", c.text);
                    fprintf(cg->body, "    testl %%eax, %%eax\n");
                    fprintf(cg->body, "    jz .Lnext%d\n", next_lbl);
                    free_if_temp(cg, &c);
                }
                emit_block(cg, br->as.if_branch.block);
                fprintf(cg->body, "    jmp .Lend%d\n", end_lbl);
                fprintf(cg->body, ".Lnext%d:\n", next_lbl);
            }
            fprintf(cg->body, ".Lend%d:\n", end_lbl);
            break;
        }
        case NODE_WHILE_STMT: {
            int top = new_label(cg), end = new_label(cg);
            fprintf(cg->body, ".Ltop%d:\n", top);
            Opnd c = emit_expr(cg, node->as.while_stmt.condition);
            call1(cg, "vo_truthy", c.text);
            fprintf(cg->body, "    testl %%eax, %%eax\n");
            fprintf(cg->body, "    jz .Lend%d\n", end);
            free_if_temp(cg, &c);
            emit_block(cg, node->as.while_stmt.block);
            fprintf(cg->body, "    jmp .Ltop%d\n", top);
            fprintf(cg->body, ".Lend%d:\n", end);
            break;
        }
        case NODE_FOR_STMT: {
            int id = cg->for_id++;
            char iter_label[40], end_label[40];
            snprintf(iter_label, sizeof(iter_label), "for_iter_%d", id);
            snprintf(end_label, sizeof(end_label), "for_end_%d", id);

            Opnd start = emit_expr(cg, node->as.for_stmt.start);
            Opnd end = emit_expr(cg, node->as.for_stmt.end);
            char iter_rip[48], end_rip[48];
            snprintf(iter_rip, sizeof(iter_rip), "%s(%%rip)", iter_label);
            snprintf(end_rip, sizeof(end_rip), "%s(%%rip)", end_label);
            call2(cg, "vo_assign", iter_rip, start.text);
            call2(cg, "vo_assign", end_rip, end.text);
            free_if_temp(cg, &start);
            free_if_temp(cg, &end);

            CScope *outer = cg->scope;
            cg->scope = scope_push(outer);
            CSym *isym = scope_declare(cg->scope, node->as.for_stmt.iterator);
            isym->is_loop_var = 1;
            snprintf(isym->loop_label, sizeof(isym->loop_label), "%s", iter_label);

            int top = new_label(cg), endl = new_label(cg);
            fprintf(cg->body, ".Ltop%d:\n", top);
            Opnd le = alloc_temp(cg);
            call3(cg, "vo_le", le.text, iter_rip, end_rip);
            call1(cg, "vo_truthy", le.text);
            free_temp(cg, &le);
            fprintf(cg->body, "    testl %%eax, %%eax\n");
            fprintf(cg->body, "    jz .Lend%d\n", endl);
            emit_block(cg, node->as.for_stmt.block);
            Opnd one = alloc_temp(cg);
            const char *oo[2] = { one.text, "1" }; int oim[2] = { 0, 1 };
            callN(cg, "vo_set_num", 2, oo, oim);
            call4_imm(cg, "vo_add", iter_rip, iter_rip, one.text, node->line);
            free_temp(cg, &one);
            fprintf(cg->body, "    jmp .Ltop%d\n", top);
            fprintf(cg->body, ".Lend%d:\n", endl);

            cg->scope = outer;
            break;
        }
        case NODE_GOTO_STMT:
            fprintf(cg->body, "    jmp vlbl_%s\n", node->as.goto_stmt.label);
            break;
        case NODE_LABEL_STMT:
            fprintf(cg->body, "vlbl_%s:\n", node->as.label_stmt.label);
            break;
        case NODE_WHEN_STMT:
            if (node->as.when_stmt.kind == WHEN_PROGRAM_START) {
                emit_block(cg, node->as.when_stmt.block); /* runs in place, in program order */
            } else {
                cg_error(cg, node->line, "WHEN <vma> CHANGED / WHEN <condition> handlers are not yet supported by "
                                          "the native compiler (voc) - use the tree-walking interpreter (voi) for "
                                          "programs that need event handlers");
            }
            break;
        case NODE_BLOCK:
            emit_block(cg, node);
            break;
        case NODE_JOB_DECL:
            /* Jobs are emitted as separate functions by codegen_compile -
               nothing to execute in the calling body. */
            break;
        case NODE_GIVE_STMT:
            emit_give_stmt(cg, node);
            break;
        case NODE_DEMAND_STMT:
            emit_demand_stmt(cg, node);
            break;
        case NODE_SERVE_STMT:
            emit_serve_stmt(cg, node);
            break;
        case NODE_DO_STMT:
            emit_do_stmt(cg, node);
            break;
        case NODE_BRING_STMT:
        case NODE_SHIP_STMT:
        case NODE_PEICE_DECL:
            /* Module statements are handled in the pre-pass; no codegen needed here */
            break;
        default:
            cg_error(cg, node->line, "this statement form is not yet supported by the native compiler");
            break;
    }
}

static void emit_block(CG *cg, ASTNode *block) {
    for (int i = 0; i < block->as.block.statements.count; i++)
        emit_stmt(cg, block->as.block.statements.items[i]);
}

/* ---- GIVE: write the current job's return slot and jump to its epilogue ---- */
static void emit_give_stmt(CG *cg, ASTNode *node) {
    if (cg->cur_job_id < 0) {
        cg_error(cg, node->line, "GIVE may only appear inside a JOB");
        return;
    }
    Opnd v = emit_expr(cg, node->as.give_stmt.value);
    char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", cg->cur_job_ret);
    call2(cg, "vo_assign", rip, v.text);
    free_if_temp(cg, &v);
    fprintf(cg->body, "    jmp Ljobret_%d\n", cg->cur_job_id);
}

/* ---- SERVE: raise an issue. Inside a DO it transfers to GRABE; otherwise fatal. ---- */
static void emit_serve_stmt(CG *cg, ASTNode *node) {
    const CDo *doctx = cg_do_top(cg);
    if (doctx) {
        char flagrip[48]; snprintf(flagrip, sizeof(flagrip), "%s(%%rip)", doctx->flag_label);
        const char *o[2] = { flagrip, "1" }; int im[2] = { 0, 1 };
        callN(cg, "vo_set_yn", 2, o, im);
        fprintf(cg->body, "    jmp .Ldoend_%d\n", doctx->end_label);
    } else {
        if (node->as.serve_stmt.value) {
            Opnd v = emit_expr(cg, node->as.serve_stmt.value);
            free_if_temp(cg, &v);
        }
        char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", node->line);
        const char *lbl = intern_string(cg, "SERVE: issue raised");
        char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl);
        const char *o[2] = { linebuf, rip }; int im[2] = { 1, 0 };
        callN(cg, "vo_error", 2, o, im);
    }
}

/* ---- DEMAND: assert a condition. Inside a DO, failure -> GRABE; else fatal. ---- */
static void emit_demand_stmt(CG *cg, ASTNode *node) {
    Opnd c = emit_expr(cg, node->as.demand_stmt.condition);
    char linebuf[32]; snprintf(linebuf, sizeof(linebuf), "%d", node->line);
    const char *o[2] = { c.text, linebuf }; int im[2] = { 0, 1 };
    callN(cg, "vo_demand_check", 2, o, im);
    int lbl = cg->label_id++;
    fprintf(cg->body, "    testl %%eax, %%eax\n");
    const CDo *doctx = cg_do_top(cg);
    if (doctx) {
        fprintf(cg->body, "    jnz .Ldemand_ok_%d\n", lbl);
        free_if_temp(cg, &c);
        char flagrip[48]; snprintf(flagrip, sizeof(flagrip), "%s(%%rip)", doctx->flag_label);
        const char *fo[2] = { flagrip, "1" }; int fim[2] = { 0, 1 };
        callN(cg, "vo_set_yn", 2, fo, fim);
        fprintf(cg->body, "    jmp .Ldoend_%d\n", doctx->end_label);
        fprintf(cg->body, ".Ldemand_ok_%d:\n", lbl);
    } else {
        fprintf(cg->body, "    jnz .Ldemand_ok_%d\n", lbl);
        free_if_temp(cg, &c);
        const char *msg = node->as.demand_stmt.message;
        const char *text = msg ? msg : "DEMAND condition not met";
        const char *lbl2 = intern_string(cg, text);
        char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", lbl2);
        const char *eo[2] = { linebuf, rip }; int eim[2] = { 1, 0 };
        callN(cg, "vo_error", 2, eo, eim);
        fprintf(cg->body, ".Ldemand_ok_%d:\n", lbl);
    }
}

/* ---- DO / GRABE / ENDDO ---- */
static void emit_do_stmt(CG *cg, ASTNode *node) {
    int id = cg->label_id++;
    char flag_label[40]; snprintf(flag_label, sizeof(flag_label), "vdoflag_%d", id);
    const char *flag_symbol = global_label_for_addr(cg, flag_label);
    char flagrip[48]; snprintf(flagrip, sizeof(flagrip), "%s(%%rip)", flag_symbol);
    const char *o[2] = { flagrip, "0" }; int im[2] = { 0, 1 };
    callN(cg, "vo_set_yn", 2, o, im);

    cg_do_push(cg, flag_symbol, id);
    if (node->as.do_stmt.try_block)
        emit_block(cg, node->as.do_stmt.try_block);
    cg_do_pop(cg);

    fprintf(cg->body, ".Ldoend_%d:\n", id);
    fprintf(cg->body, "    leaq %s(%%rip), %%rcx\n", flag_symbol);
    call0(cg, "vo_truthy");
    fprintf(cg->body, "    testl %%eax, %%eax\n");
    fprintf(cg->body, "    jz .Ldoskip_%d\n", id);
    if (node->as.do_stmt.catch_block)
        emit_block(cg, node->as.do_stmt.catch_block);
    fprintf(cg->body, ".Ldoskip_%d:\n", id);
}

/* =========================================================================
 * Top level driver
 * ========================================================================= */
int codegen_compile(ASTNode *program, CgTarget target, const char *source_dir, FILE *out) {
    CG cg; memset(&cg, 0, sizeof(cg));
    cg.target = target;
    cg.abi = abi_for(target);
    cg.scope = scope_push(NULL);
    cg.cur_job_id = -1;
    cg.body = open_memstream(&cg.body_buf, &cg.body_len);
    cg.rodata = open_memstream(&cg.rodata_buf, &cg.rodata_len);
    cg.source_dir = source_dir;

    /* ---- Module loading pass ----
       Walk top-level statements to find BRINGs and load modules.
       The root node from parser_parse_program() is NODE_PROGRAM (nested
       blocks, e.g. a PEICE body, are NODE_BLOCK) - both share the same
       `as.block.statements` layout, so accept either here. */
    if (program->type == NODE_BLOCK || program->type == NODE_PROGRAM) {
        for (int i = 0; i < program->as.block.statements.count; i++) {
            ASTNode *stmt = program->as.block.statements.items[i];
            if (stmt && stmt->type == NODE_BRING_STMT) {
                if (cg_module_load(&cg, stmt->as.bring_stmt.path, source_dir)) {
                    cg.had_error = 1;
                    break;
                }
            }
        }
    }
    if (cg.had_error) { free(cg.body_buf); free(cg.rodata_buf); return 1; }

    /* ---- Pre-pass: register every top-level JOB and module jobs ---- */
    for (int i = 0; i < program->as.block.statements.count; i++) {
        ASTNode *stmt = program->as.block.statements.items[i];
        if (!stmt || stmt->type != NODE_JOB_DECL) continue;
        int id = cg.label_id++;
        CJob *job = cg_job_add(&cg, stmt, id, stmt->as.job_decl.name, NULL);
        if (!job) continue;
        int pc = stmt->as.job_decl.param_count;
        if (pc > 16) { cg_error(&cg, stmt->line, "too many job parameters (max 16)"); continue; }
        job->param_count = pc;
        for (int p = 0; p < pc; p++) {
            char addr[40]; snprintf(addr, sizeof(addr), "jobp_%d_%d", id, p);
            snprintf(job->param_labels[p], sizeof(job->param_labels[p]), "%s",
                     global_label_for_addr(&cg, addr));
        }
        char retaddr[40]; snprintf(retaddr, sizeof(retaddr), "vjobret_%d", id);
        snprintf(job->ret_label, sizeof(job->ret_label), "%s",
                 global_label_for_addr(&cg, retaddr));
    }

    /* ---- Register module jobs (prefixed) ---- */
    for (int m = 0; m < cg.module_count; m++) {
        CgModule *mod = &cg.modules[m];
        if (!mod->loaded || !mod->peice_ast) continue;
        ASTNode *body = mod->peice_ast->as.peice_decl.body;
        if (body && body->type == NODE_BLOCK) {
            for (int i = 0; i < body->as.block.statements.count; i++) {
                ASTNode *stmt = body->as.block.statements.items[i];
                if (!stmt || stmt->type != NODE_JOB_DECL) continue;
                int id = cg.label_id++;
                char qualified[128];
                snprintf(qualified, sizeof(qualified), "%s#%s", mod->name, stmt->as.job_decl.name);
                CJob *job = cg_job_add(&cg, stmt, id, qualified, mod->name);
                if (!job) { cg_error(&cg, stmt->line, "duplicate job name within module"); continue; }
                int pc = stmt->as.job_decl.param_count;
                if (pc > 16) { cg_error(&cg, stmt->line, "too many job parameters (max 16)"); continue; }
                job->param_count = pc;
                for (int p = 0; p < pc; p++) {
                    char addr[40]; snprintf(addr, sizeof(addr), "jobp_%d_%d", id, p);
                    snprintf(job->param_labels[p], sizeof(job->param_labels[p]), "%s",
                             global_label_for_addr(&cg, addr));
                }
                char retaddr[40]; snprintf(retaddr, sizeof(retaddr), "vjobret_%d", id);
                snprintf(job->ret_label, sizeof(job->ret_label), "%s",
                         global_label_for_addr(&cg, retaddr));
                /* Register this job in the module's member list if SHIPped */
                for (int mm = 0; mm < cg.modules[m].member_count; mm++) {
                    CgModMember *mmb = &cg.modules[m].members[mm];
                    if (mmb->kind == CG_MOD_MEMBER_JOB && mmb->is_exported &&
                        strcmp(mmb->name, stmt->as.job_decl.name) == 0) {
                        mmb->job_internal = strdup(qualified);
                        break;
                    }
                }
            }
        }
    }

    /* ---- Initialize SHIPped module variables ----
       Module member vars (mm->var_label) get no storage until now: walk
       each loaded module's own top-level VAR/HARD decls, allocate a .bss
       slot for every SHIPped one, and emit its initializer into vo_main's
       body (which cg.body still points at here) so it runs before any
       reference to module.MEMBER. Non-exported module-level vars are left
       alone - they're only usable from inside the module's own job bodies,
       which isn't wired up yet and isn't needed for math.vo-style modules. */
    for (int m = 0; m < cg.module_count; m++) {
        CgModule *mod = &cg.modules[m];
        if (!mod->loaded || !mod->peice_ast) continue;
        ASTNode *body = mod->peice_ast->as.peice_decl.body;
        if (!body || body->type != NODE_BLOCK) continue;
        for (int i = 0; i < body->as.block.statements.count; i++) {
            ASTNode *stmt = body->as.block.statements.items[i];
            if (!stmt || (stmt->type != NODE_VAR_DECL && stmt->type != NODE_HARD_DECL)) continue;
            const char *var_name = stmt->as.var_decl.name; /* var_decl/hard_decl share layout */
            CgModMember *mm = cg_module_member_find(mod, var_name);
            if (!mm || mm->kind != CG_MOD_MEMBER_VAR || !mm->is_exported) continue;

            CVmaSlot *slot = vma_alloc_next(&cg.vmas);
            const char *label = global_label_for_addr(&cg, slot->address);
            snprintf(mm->var_label, sizeof(mm->var_label), "%s", label);

            Opnd init = emit_expr(&cg, stmt->as.var_decl.init);
            char rip[48]; snprintf(rip, sizeof(rip), "%s(%%rip)", label);
            call2(&cg, "vo_assign", rip, init.text);
            free_if_temp(&cg, &init);
        }
    }

    /* ---- emit vo_main body (JOB decls are no-ops here) ---- */
    int saved_for_id_start = cg.for_id;
    emit_block(&cg, program);
    MEMSTREAM_CLOSE(cg.body, &cg.body_buf, &cg.body_len);

    if (cg.had_error) { free(cg.body_buf); free(cg.rodata_buf); return 1; }

    fprintf(out, "# Generated by voc (Virtual Order native compiler). Do not edit by hand.\n");
    fprintf(out, "    .text\n");

    /* ---- write vo_main ---- */
    {
        long frame_bytes = (long)(cg.max_depth + 1) * SLOT_SIZE + cg.abi.shadow_space + 8L * cg.max_stack_args;
        frame_bytes = (frame_bytes + 15) & ~15L;
        fprintf(out, "    .globl vo_main\nvo_main:\n    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $%ld, %%rsp\n", frame_bytes);
        fprintf(out, "%s", cg.body_buf);
        fprintf(out, "    leave\n    ret\n\n");
        free(cg.body_buf);
    }

    /* ---- write each JOB as its own function ---- */
    for (int j = 0; j < cg.job_count; j++) {
        CJob *job = &cg.jobs[j];
        int pc = job->param_count;

        cg.cur_job_id = job->id;
        cg.cur_job_ret = job->ret_label;
        cg.cur_job_module = job->module_name;
        cg.depth = 0;
        cg.max_depth = 0;
        cg.body = open_memstream(&cg.body_buf, &cg.body_len);

        CScope *saved_scope = cg.scope;
        cg.scope = scope_push(saved_scope);
        for (int p = 0; p < pc; p++) {
            CSym *sym = scope_declare(cg.scope, job->decl->as.job_decl.params[p].name);
            char addr[40]; snprintf(addr, sizeof(addr), "jobp_%d_%d", job->id, p);
            snprintf(sym->vma, sizeof(sym->vma), "%s", addr);
        }

        emit_block(&cg, job->decl->as.job_decl.body);
        fprintf(cg.body, "Ljobret_%d:\n", job->id);
        MEMSTREAM_CLOSE(cg.body, &cg.body_buf, &cg.body_len);
        cg.scope = saved_scope;
        cg.cur_job_id = -1;
        cg.cur_job_ret = NULL;
        cg.cur_job_module = NULL;

        if (cg.had_error) break;

        long jframe = (long)(cg.max_depth + 1) * SLOT_SIZE + cg.abi.shadow_space + 8L * cg.max_stack_args;
        jframe = (jframe + 15) & ~15L;
        fprintf(out, "    .globl vo_job_%d\nvo_job_%d:\n    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $%ld, %%rsp\n", job->id, job->id, jframe);
        fprintf(out, "%s", cg.body_buf);
        fprintf(out, "    leave\n    ret\n\n");
        free(cg.body_buf);
    }

    if (cg.had_error) { free(cg.rodata_buf); return 1; }

    int total_for_loops = cg.for_id - saved_for_id_start;

    MEMSTREAM_CLOSE(cg.rodata, &cg.rodata_buf, &cg.rodata_len);

    fprintf(out, "    .section .rodata\n");
    fprintf(out, "%s", cg.rodata_buf);
    fprintf(out, "\n");

    /* job name string constants (referenced by the runtime job table) */
    for (int i = 0; i < cg.job_count; i++)
        fprintf(out, "vjobname_%d:\n    .string \"%s\"\n", cg.jobs[i].id, cg.jobs[i].name);

    fprintf(out, "    .bss\n");
    for (int i = 0; i < cg.gcount; i++)
        fprintf(out, "    .align 8\n%s:\n    .zero %ld\n", cg.globals[i].label, SLOT_SIZE);
    for (int i = 0; i < total_for_loops; i++) {
        fprintf(out, "    .align 8\nfor_iter_%d:\n    .zero %ld\n", i, SLOT_SIZE);
        fprintf(out, "    .align 8\nfor_end_%d:\n    .zero %ld\n", i, SLOT_SIZE);
    }

    /* module vars: storage went to .bss alongside the module member's slot
       guard; job param/ret slots are also emitted into .bss above. */

    /* ---- runtime job table (vo_job_tab / vo_job_tab_count) ----
       Emit unconditionally: the overlaid runtime object always contains a
       relocation against vo_job_tab (from the generic vo_task_run /
       vo_cmd_spawn helpers), and the static linker needs the symbol even
       for programs that declare no jobs. Empty table is fine - job_index is
       only ever read after a SPAWN, which requires at least one job. */
    fprintf(out, "    .section .data\n");
    fprintf(out, "    .globl vo_job_tab\n");
    fprintf(out, "    .align 8\n");
    fprintf(out, "vo_job_tab:\n");
    for (int j = 0; j < cg.job_count; j++) {
        CJob *job = &cg.jobs[j];
        fprintf(out, "    .quad vjobname_%d\n", job->id);
        fprintf(out, "    .quad %d\n", job->param_count);
        fprintf(out, "    .quad vo_job_%d\n", job->id);
        for (int p = 0; p < 16; p++) {
            if (p < job->param_count) fprintf(out, "    .quad %s\n", job->param_labels[p]);
            else fprintf(out, "    .quad 0\n");
        }
        fprintf(out, "    .quad %s\n", job->ret_label);
    }
    fprintf(out, "    .globl vo_job_tab_count\n");
    fprintf(out, "    .align 4\n");
    fprintf(out, "vo_job_tab_count:\n");
    fprintf(out, "    .long %d\n", cg.job_count);

    free(cg.rodata_buf);
    free(cg.jobs);
    free(cg.do_stack);
    free(cg.modules);
    return 0;
}
