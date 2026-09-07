#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <setjmp.h>
#include "runtime.h"
#include "parser.h"
#include "ast.h"

/* =======================================================================
 * Values
 * ===================================================================== */
typedef enum { VAL_NUM, VAL_DEC, VAL_TEX, VAL_YN, VAL_COLL, VAL_EMP } ValType;

typedef struct Value {
    ValType type;
    long num;
    double dec;
    char *tex;
    int yn;
    struct Value *items;
    int count, capacity;
} Value;

static Value value_num(long n)  { Value v = {0}; v.type = VAL_NUM; v.num = n; return v; }
static Value value_dec(double d){ Value v = {0}; v.type = VAL_DEC; v.dec = d; return v; }
static Value value_yn(int b)    { Value v = {0}; v.type = VAL_YN; v.yn = b ? 1 : 0; return v; }
static Value value_emp(void)    { Value v = {0}; v.type = VAL_EMP; return v; }
static Value value_tex(const char *s) { Value v = {0}; v.type = VAL_TEX; v.tex = strdup(s ? s : ""); return v; }
static Value value_coll_empty(void) { Value v = {0}; v.type = VAL_COLL; return v; }

static void value_free(Value *v) {
    if (!v) return;
    if (v->type == VAL_TEX) { free(v->tex); v->tex = NULL; }
    if (v->type == VAL_COLL) {
        for (int i = 0; i < v->count; i++) value_free(&v->items[i]);
        free(v->items);
        v->items = NULL; v->count = v->capacity = 0;
    }
}

static Value value_copy(Value v) {
    Value out = v;
    if (v.type == VAL_TEX) out.tex = strdup(v.tex ? v.tex : "");
    if (v.type == VAL_COLL) {
        out.items = v.count ? malloc(sizeof(Value) * v.count) : NULL;
        out.capacity = v.count;
        for (int i = 0; i < v.count; i++) out.items[i] = value_copy(v.items[i]);
    }
    return out;
}

static void value_coll_push(Value *coll, Value item) {
    if (coll->count == coll->capacity) {
        coll->capacity = coll->capacity ? coll->capacity * 2 : 4;
        coll->items = realloc(coll->items, sizeof(Value) * coll->capacity);
    }
    coll->items[coll->count++] = item;
}

static char *value_to_cstr(Value v) {
    char buf[64];
    switch (v.type) {
        case VAL_NUM: snprintf(buf, sizeof(buf), "%ld", v.num); return strdup(buf);
        case VAL_DEC: snprintf(buf, sizeof(buf), "%g", v.dec); return strdup(buf);
        case VAL_TEX: return strdup(v.tex ? v.tex : "");
        case VAL_YN:  return strdup(v.yn ? "YES" : "NO");
        case VAL_EMP: return strdup("EMP");
        case VAL_COLL: {
            size_t cap = 64, len = 0;
            char *out = malloc(cap);
            out[0] = '\0';
            len = 1;
            strcpy(out, "[");
            len = 1;
            for (int i = 0; i < v.count; i++) {
                char *piece = value_to_cstr(v.items[i]);
                size_t plen = strlen(piece);
                while (len + plen + 4 > cap) { cap *= 2; out = realloc(out, cap); }
                if (i > 0) { strcpy(out + len - 1, ", "); len += 1; out[len] = '\0'; }
                strcpy(out + len - 1, piece);
                len += plen;
                free(piece);
            }
            if (len + 2 > cap) { cap += 2; out = realloc(out, cap); }
            out[len - 1] = ']';
            out[len] = '\0';
            return out;
        }
    }
    return strdup("");
}

static int value_to_bool(Value v) {
    switch (v.type) {
        case VAL_NUM: return v.num != 0;
        case VAL_DEC: return v.dec != 0.0;
        case VAL_TEX: return v.tex && v.tex[0] != '\0';
        case VAL_YN:  return v.yn != 0;
        case VAL_COLL: return v.count != 0;
        case VAL_EMP: return 0;
    }
    return 0;
}

static double value_as_double(Value v) { return v.type == VAL_DEC ? v.dec : (double)v.num; }
static long    value_as_long(Value v)  { return v.type == VAL_DEC ? (long)v.dec : v.num; }
static int     value_is_numeric(Value v) { return v.type == VAL_NUM || v.type == VAL_DEC; }

static int value_equal(Value a, Value b) {
    if (value_is_numeric(a) && value_is_numeric(b)) return value_as_double(a) == value_as_double(b);
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_TEX: return strcmp(a.tex ? a.tex : "", b.tex ? b.tex : "") == 0;
        case VAL_YN:  return a.yn == b.yn;
        case VAL_EMP: return 1;
        case VAL_COLL:
            if (a.count != b.count) return 0;
            for (int i = 0; i < a.count; i++) if (!value_equal(a.items[i], b.items[i])) return 0;
            return 1;
        default: return 0;
    }
}

/* =======================================================================
 * Diagnostics / abort-on-runtime-error
 * ===================================================================== */
typedef struct VM VM;
static void runtime_error(VM *vm, int line, const char *fmt, ...);

/* =======================================================================
 * Scoped symbol table
 * ===================================================================== */
typedef struct {
    char *name;
    int is_const;
    int is_loop_var;
    char vma[16];
    Value direct_value;
    int autocleans;
} Symbol;

typedef struct Scope {
    Symbol *symbols;
    int count, capacity;
    struct Scope *parent;
} Scope;

static Scope *scope_push(Scope *parent) {
    Scope *s = calloc(1, sizeof(Scope));
    s->parent = parent;
    return s;
}

static Symbol *scope_find_local(Scope *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->symbols[i].name, name) == 0) return &s->symbols[i];
    return NULL;
}

static Symbol *scope_resolve(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        Symbol *sym = scope_find_local(s, name);
        if (sym) return sym;
    }
    return NULL;
}

static Symbol *scope_declare(Scope *s, const char *name) {
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 4;
        s->symbols = realloc(s->symbols, sizeof(Symbol) * s->capacity);
    }
    Symbol *sym = &s->symbols[s->count++];
    memset(sym, 0, sizeof(Symbol));
    sym->name = strdup(name);
    return sym;
}

static void scope_pop_last(Scope *s) {
    if (s->count > 0) {
        Symbol *sym = &s->symbols[s->count - 1];
        if (sym->is_loop_var) value_free(&sym->direct_value);
        free(sym->name);
        s->count--;
    }
}

/* =======================================================================
 * VMA table
 * ===================================================================== */
#define VMA_NUMBERS_PER_LETTER 9999

typedef struct {
    long index;
    char address[16];
    int allocated;
    Value value;
    char *owner;
} RVmaSlot;

typedef struct {
    RVmaSlot *slots;
    int count, capacity;
} RVmaTable;

static long letters_to_index(const char *s, int len) {
    long idx = 0;
    for (int i = 0; i < len; i++) idx = idx * 26 + (s[i] - 'A' + 1);
    return idx;
}

static int is_vma_format(const char *s) {
    int i = 0;
    while (isupper((unsigned char)s[i])) i++;
    if (i == 0 || s[i] == '\0') return 0;
    int j = i;
    while (isdigit((unsigned char)s[j])) j++;
    return j > i && s[j] == '\0';
}

static long vma_canonical_index(const char *vma) {
    int i = 0;
    while (isupper((unsigned char)vma[i])) i++;
    long letter_idx = letters_to_index(vma, i);
    long number = strtol(vma + i, NULL, 10);
    return (letter_idx - 1) * VMA_NUMBERS_PER_LETTER + number;
}

static void index_to_vma(long index, char *out) {
    long number = ((index - 1) % VMA_NUMBERS_PER_LETTER) + 1;
    long letter_idx = ((index - 1) / VMA_NUMBERS_PER_LETTER) + 1;
    char letters[16];
    int li = 0;
    while (letter_idx > 0) {
        long rem = (letter_idx - 1) % 26;
        letters[li++] = (char)('A' + rem);
        letter_idx = (letter_idx - 1) / 26;
    }
    int oi = 0;
    for (int i = li - 1; i >= 0; i--) out[oi++] = letters[i];
    sprintf(out + oi, "%ld", number);
}

static RVmaSlot *rvma_slot_for_index(RVmaTable *t, long idx) {
    for (int i = 0; i < t->count; i++)
        if (t->slots[i].index == idx) return &t->slots[i];
    if (t->count == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 8;
        t->slots = realloc(t->slots, sizeof(RVmaSlot) * t->capacity);
    }
    RVmaSlot *slot = &t->slots[t->count++];
    memset(slot, 0, sizeof(RVmaSlot));
    slot->index = idx;
    slot->value = value_emp();
    index_to_vma(idx, slot->address);
    return slot;
}

static RVmaSlot *rvma_lookup(RVmaTable *t, const char *addr) {
    long idx = vma_canonical_index(addr);
    for (int i = 0; i < t->count; i++)
        if (t->slots[i].index == idx) return &t->slots[i];
    return NULL;
}

static RVmaSlot *rvma_alloc_next(RVmaTable *t, const char *owner) {
    long idx = 1;
    for (;;) {
        RVmaSlot *existing = NULL;
        for (int i = 0; i < t->count; i++)
            if (t->slots[i].index == idx) { existing = &t->slots[i]; break; }
        if (!existing || !existing->allocated) break;
        idx++;
    }
    RVmaSlot *slot = rvma_slot_for_index(t, idx);
    slot->allocated = 1;
    free(slot->owner);
    slot->owner = owner ? strdup(owner) : NULL;
    return slot;
}

static void rvma_free(RVmaTable *t, const char *addr) {
    RVmaSlot *slot = rvma_lookup(t, addr);
    if (!slot) return;
    slot->allocated = 0;
    value_free(&slot->value);
    slot->value = value_emp();
    free(slot->owner);
    slot->owner = NULL;
}

static void rvma_free_all(RVmaTable *t) {
    for (int i = 0; i < t->count; i++) {
        t->slots[i].allocated = 0;
        value_free(&t->slots[i].value);
        t->slots[i].value = value_emp();
        free(t->slots[i].owner);
        t->slots[i].owner = NULL;
    }
}

/* =======================================================================
 * Module system
 * ===================================================================== */
typedef enum { MOD_MEMBER_JOB, MOD_MEMBER_VAR } ModMemberKind;

typedef struct {
    char *name;                /* exported name (e.g., "ADD") */
    ModMemberKind kind;        /* job or variable */
    char *job_internal;        /* for jobs: internal job name (e.g., "math#ADD") */
    RVmaSlot *slot;            /* for vars: pointer to VMA slot */
    int is_exported;           /* SHIPped? */
} ModMember;

typedef struct {
    char *name;                /* module name (e.g., "math") */
    ModMember *members; int member_count, member_cap;
    int loaded;                /* 1 if fully loaded */
    int loading;               /* 1 if currently being loaded (for cycle detection) */
    ASTNode *peice_ast;        /* the PEICE_DECL node for this module (for compilation) */
    Scope *persistent_scope;   /* scope for SHIPped variables, persists after init */
} Module;

/* =======================================================================
 * Job registry
 * ===================================================================== */
typedef struct {
    char *name;
    int body_start;
    int param_count;
    struct { char *name; VOTokenType var_type; } *params;
} JobEntry;

/* =======================================================================
 * WHEN handlers + FIFO event queue
 * ===================================================================== */
typedef struct {
    WhenKind kind;
    char vma_addr[16];
    ASTNode *condition;
    int last_state;
    int body_start;
} Handler;

typedef struct {
    int handler_index;
    int has_values;
    Value old_value, new_value;
} QueueEntry;

#define DEFAULT_MAX_QUEUE_DEPTH 1000

/* =======================================================================
 * Flattened, jump-based instruction stream
 * ===================================================================== */
typedef enum {
    I_STMT, I_JUMP, I_JUMP_IF_FALSE,
    I_FOR_SETUP, I_FOR_TEST, I_FOR_STEP, I_FOR_TEARDOWN,
    I_SCOPE_PUSH, I_SCOPE_POP,
    I_WHEN_REGISTER, I_HANDLER_RETURN,
    I_GIVE
} InstrKind;

typedef struct {
    InstrKind kind;
    ASTNode *node;
    int target;
    int target2;
} Instr;

typedef struct { Instr *items; int count, capacity; } InstrList;

typedef struct { char *name; int index; } LabelEntry;
typedef struct { int instr_index; char *label; int line; } PendingGoto;

typedef struct {
    InstrList instrs;
    LabelEntry *labels; int label_count, label_cap;
    PendingGoto *pending; int pending_count, pending_cap;
    JobEntry *jobs; int job_count, job_cap;
    int had_error;
    const char *module_prefix;   /* prefix for job names when inside a module (e.g., "math#") */
} Compiler;

static int emit(Compiler *c, InstrKind kind, ASTNode *node) {
    if (c->instrs.count == c->instrs.capacity) {
        c->instrs.capacity = c->instrs.capacity ? c->instrs.capacity * 2 : 64;
        c->instrs.items = realloc(c->instrs.items, sizeof(Instr) * c->instrs.capacity);
    }
    Instr *ins = &c->instrs.items[c->instrs.count];
    ins->kind = kind; ins->node = node; ins->target = -1; ins->target2 = -1;
    return c->instrs.count++;
}

static void patch(Compiler *c, int idx, int target) { c->instrs.items[idx].target = target; }

static void label_define(Compiler *c, const char *name, int index, int line) {
    for (int i = 0; i < c->label_count; i++)
        if (strcmp(c->labels[i].name, name) == 0) {
            fprintf(stderr, "[line %d] Runtime error: label '%s' is already defined elsewhere in this program\n", line, name);
            c->had_error = 1;
            return;
        }
    if (c->label_count == c->label_cap) {
        c->label_cap = c->label_cap ? c->label_cap * 2 : 8;
        c->labels = realloc(c->labels, sizeof(LabelEntry) * c->label_cap);
    }
    c->labels[c->label_count].name = strdup(name);
    c->labels[c->label_count].index = index;
    c->label_count++;
}

static void pending_goto_add(Compiler *c, int instr_index, const char *label, int line) {
    if (c->pending_count == c->pending_cap) {
        c->pending_cap = c->pending_cap ? c->pending_cap * 2 : 8;
        c->pending = realloc(c->pending, sizeof(PendingGoto) * c->pending_cap);
    }
    c->pending[c->pending_count].instr_index = instr_index;
    c->pending[c->pending_count].label = strdup(label);
    c->pending[c->pending_count].line = line;
    c->pending_count++;
}

static void job_register(Compiler *c, const char *name, int body_start, JobParam *params, int param_count) {
    if (c->job_count == c->job_cap) {
        c->job_cap = c->job_cap ? c->job_cap * 2 : 8;
        c->jobs = realloc(c->jobs, sizeof(JobEntry) * c->job_cap);
    }
    JobEntry *j = &c->jobs[c->job_count++];
    /* Apply module prefix if inside a module */
    if (c->module_prefix) {
        char prefixed[128];
        snprintf(prefixed, sizeof(prefixed), "%s%s", c->module_prefix, name);
        j->name = strdup(prefixed);
    } else {
        j->name = strdup(name);
    }
j->body_start = body_start;
    j->param_count = param_count;
    j->params = param_count ? malloc(sizeof(*j->params) * param_count) : NULL;
    for (int i = 0; i < param_count; i++) {
        j->params[i].name = strdup(params[i].name);
        j->params[i].var_type = params[i].var_type;
    }
}

static void compile_block(Compiler *c, ASTNode *block);

/* Module system forward declarations */
static Module *module_find(VM *vm, const char *name);
static Module *module_ensure(VM *vm, const char *name);
static ModMember *module_member_find(Module *mod, const char *name);
static ModMember *module_member_add(Module *mod, const char *name, ModMemberKind kind);
static int module_load(VM *vm, const char *path, const char *base_dir);
static void module_compile_all(Compiler *c, VM *vm);
static void module_finalize_jobs(VM *vm, Module *mod);
static int module_initialize(VM *vm, Module *mod);
static void exec_simple_stmt(VM *vm, ASTNode *node);

static void compile_stmt(Compiler *c, ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_VAR_DECL: case NODE_HARD_DECL:
        case NODE_EXPR_STMT: case NODE_INC_DEC_STMT:
        case NODE_SHOW_STMT: case NODE_STORE_STMT:
        case NODE_CLEAN_STMT: case NODE_CLEANALL_STMT:
        case NODE_AUTOCLEAN_STMT:
        case NODE_DEMAND_STMT: case NODE_SERVE_STMT:
            emit(c, I_STMT, node);
            break;

        case NODE_GIVE_STMT:
            emit(c, I_GIVE, node);
            break;

        case NODE_LABEL_STMT:
            label_define(c, node->as.label_stmt.label, c->instrs.count, node->line);
            break;

        case NODE_GOTO_STMT: {
            int idx = emit(c, I_JUMP, node);
            pending_goto_add(c, idx, node->as.goto_stmt.label, node->line);
            break;
        }

        case NODE_IF_STMT: {
            int *end_jumps = malloc(sizeof(int) * node->as.if_stmt.branches.count);
            int n_end = 0;
            int prev_false_jump = -1;
            for (int i = 0; i < node->as.if_stmt.branches.count; i++) {
                ASTNode *branch = node->as.if_stmt.branches.items[i];
                if (prev_false_jump >= 0) patch(c, prev_false_jump, c->instrs.count);
                if (branch->as.if_branch.condition) {
                    int jf = emit(c, I_JUMP_IF_FALSE, branch->as.if_branch.condition);
                    compile_block(c, branch->as.if_branch.block);
                    end_jumps[n_end++] = emit(c, I_JUMP, NULL);
                    prev_false_jump = jf;
                } else {
                    compile_block(c, branch->as.if_branch.block);
                    prev_false_jump = -1;
                }
            }
            if (prev_false_jump >= 0) patch(c, prev_false_jump, c->instrs.count);
            for (int i = 0; i < n_end; i++) patch(c, end_jumps[i], c->instrs.count);
            free(end_jumps);
            break;
        }

        case NODE_WHILE_STMT: {
            int loop_start = c->instrs.count;
            int jf = emit(c, I_JUMP_IF_FALSE, node->as.while_stmt.condition);
            compile_block(c, node->as.while_stmt.block);
            emit(c, I_JUMP, NULL);
            c->instrs.items[c->instrs.count - 1].target = loop_start;
            patch(c, jf, c->instrs.count);
            break;
        }

        case NODE_FOR_STMT: {
            emit(c, I_FOR_SETUP, node);
            int loop_start = c->instrs.count;
            int test = emit(c, I_FOR_TEST, node);
            compile_block(c, node->as.for_stmt.block);
            emit(c, I_FOR_STEP, node);
            int back = emit(c, I_JUMP, NULL);
            patch(c, back, loop_start);
            patch(c, test, c->instrs.count);
            emit(c, I_FOR_TEARDOWN, node);
            break;
        }

        case NODE_WHEN_STMT: {
            if (node->as.when_stmt.kind == WHEN_PROGRAM_START) {
                emit(c, I_SCOPE_PUSH, NULL);
                compile_block(c, node->as.when_stmt.block);
                emit(c, I_SCOPE_POP, NULL);
            } else {
                int reg = emit(c, I_WHEN_REGISTER, node);
                int skip = emit(c, I_JUMP, NULL);
                int body_start = c->instrs.count;
                emit(c, I_SCOPE_PUSH, NULL);
                compile_block(c, node->as.when_stmt.block);
                emit(c, I_SCOPE_POP, NULL);
                emit(c, I_HANDLER_RETURN, NULL);
                patch(c, skip, c->instrs.count);
                c->instrs.items[reg].target2 = body_start;
            }
            break;
        }

        case NODE_JOB_DECL: {
            /* Skip over job body in normal execution flow */
            int skip = emit(c, I_JUMP, NULL);
            int body_start = c->instrs.count;
            /* Job body: compile statements, end with I_HANDLER_RETURN */
            compile_block(c, node->as.job_decl.body);
            emit(c, I_HANDLER_RETURN, NULL);
            patch(c, skip, c->instrs.count);
            /* Register the job */
            job_register(c, node->as.job_decl.name, body_start,
                         node->as.job_decl.params, node->as.job_decl.param_count);
            break;
        }

        case NODE_DO_STMT: {
            /* DO/GRABE: compile try block, then catch block.
               SERVE inside the try block will longjmp to the catch block.
               For the instruction stream, we compile both blocks inline.
               The try block is guarded by a setjmp; if SERVE fires inside,
               it longjmps to the catch block start. We represent this
               as: compile try block normally, then compile catch block.
               The runtime handles the actual catch via do_depth/do_catch_pc. */
            emit(c, I_STMT, node);  /* handled in exec_simple_stmt */
            break;
        }

        case NODE_BRING_STMT:
        case NODE_SHIP_STMT:
        case NODE_PEICE_DECL:
            /* Module system - handled at runtime as stubs */
            emit(c, I_STMT, node);
            break;

        case NODE_BLOCK:
            compile_block(c, node);
            break;

        default:
            break;
    }
}

static void compile_block(Compiler *c, ASTNode *block) {
    if (!block) return;
    for (int i = 0; i < block->as.block.statements.count; i++)
        compile_stmt(c, block->as.block.statements.items[i]);
}

static void compile_program(Compiler *c, VM *vm, ASTNode *program, const char *source_dir, InstrList *out) {
    memset(c, 0, sizeof(*c));
    memset(out, 0, sizeof(*out)); /* always leave *out in a free()-able state, even on early return below */

    /* ---- Module loading pass ----
       Walk top-level statements to find BRINGs and load modules.
       The root node from parser_parse_program() is NODE_PROGRAM (nested
       blocks are NODE_BLOCK) - both share the same `as.block.statements`
       layout, so accept either here. */
    if (program->type == NODE_BLOCK || program->type == NODE_PROGRAM) {
        for (int i = 0; i < program->as.block.statements.count; i++) {
            ASTNode *stmt = program->as.block.statements.items[i];
            if (stmt && stmt->type == NODE_BRING_STMT) {
                if (module_load(vm, stmt->as.bring_stmt.path, source_dir)) {
                    c->had_error = 1;
                    return;
                }
            }
        }
    }

    /* ---- Compile each loaded module's PEICE body ---- */
    module_compile_all(c, vm);

    /* ---- Compile main program body ---- */
    compile_block(c, program);

    for (int i = 0; i < c->pending_count; i++) {
        int target = -1;
        for (int j = 0; j < c->label_count; j++)
            if (strcmp(c->labels[j].name, c->pending[i].label) == 0) { target = c->labels[j].index; break; }
        if (target < 0) {
            fprintf(stderr, "[line %d] Runtime error: GOTO target '%s' is not defined anywhere in this program\n",
                    c->pending[i].line, c->pending[i].label);
            c->had_error = 1;
        } else {
            patch(c, c->pending[i].instr_index, target);
        }
    }
    *out = c->instrs;
}

/* =======================================================================
 * VM state
 * ===================================================================== */
struct VM {
    RVmaTable vmas;
    Scope *scope;
    int autoclean_on;

    Handler *handlers; int handler_count, handler_capacity;
    QueueEntry *queue; int q_count, q_capacity;

    int in_changed_handler;
    Value cur_old_value, cur_new_value;

    Instr *code;
    int code_count;

    JobEntry *jobs;
    int job_count;

    /* Module system */
    Module *modules; int module_count, module_cap;
    int active_module;         /* index of module whose job is currently running (-1 = main) */
    int initializing_module;   /* module currently being initialized (-1 = none) */
    char *source_dir;          /* directory of the main source file for BRING resolution */

    /* GIVE mechanism */
    Value give_value;
    int giving;

    /* DO/GRABE mechanism */
    int do_depth;
    int do_catch_pc;
    jmp_buf do_catch_buf;

    long for_end_stack[256];
    int for_end_top;

    int max_queue_depth;
    int had_runtime_error;
    jmp_buf abort_buf;
};

/* =======================================================================
 * Module system helpers
 * ===================================================================== */

static Module *module_find(VM *vm, const char *name) {
    for (int i = 0; i < vm->module_count; i++)
        if (strcmp(vm->modules[i].name, name) == 0)
            return &vm->modules[i];
    return NULL;
}

static Module *module_ensure(VM *vm, const char *name) {
    Module *m = module_find(vm, name);
    if (m) return m;
    if (vm->module_count == vm->module_cap) {
        vm->module_cap = vm->module_cap ? vm->module_cap * 2 : 8;
        vm->modules = realloc(vm->modules, sizeof(Module) * vm->module_cap);
    }
    Module *nm = &vm->modules[vm->module_count++];
    memset(nm, 0, sizeof(Module));
    nm->name = strdup(name);
    return nm;
}

static ModMember *module_member_find(Module *mod, const char *name) {
    for (int i = 0; i < mod->member_count; i++)
        if (strcmp(mod->members[i].name, name) == 0)
            return &mod->members[i];
    return NULL;
}

static ModMember *module_member_add(Module *mod, const char *name, ModMemberKind kind) {
    if (mod->member_count == mod->member_cap) {
        mod->member_cap = mod->member_cap ? mod->member_cap * 2 : 8;
        mod->members = realloc(mod->members, sizeof(ModMember) * mod->member_cap);
    }
    ModMember *mm = &mod->members[mod->member_count++];
    memset(mm, 0, sizeof(ModMember));
    mm->name = strdup(name);
    mm->kind = kind;
    return mm;
}

/* Load a module from a .vo file at the given path, relative to base_dir.
   Returns 0 on success, 1 on error (error reported via runtime_error). */
static int module_load(VM *vm, const char *path, const char *base_dir) {
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
        runtime_error(vm, 0, "module file not found: %s", full_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(size + 1);
    if (fread(source, 1, size, f) != (size_t)size) { free(source); fclose(f); runtime_error(vm, 0, "failed to read module: %s", full_path); return 1; }
    source[size] = '\0';
    fclose(f);

    /* Parse */
    Parser parser;
    parser_init(&parser, source);
    ASTNode *prog = parser_parse_program(&parser);
    if (parser.had_error) {
        free(source);
        runtime_error(vm, 0, "module parse errors");
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
        runtime_error(vm, 0, "module must contain exactly one PEICE declaration");
        return 1;
    }
    ASTNode *peice = prog->as.block.statements.items[0];
    const char *peice_name = peice->as.peice_decl.name;

    /* Check module already loaded */
    Module *mod = module_find(vm, peice_name);
    if (mod) {
        if (mod->loading) {
            free(source);
            ast_free(prog);
            runtime_error(vm, 0, "circular module dependency: %s", peice_name);
            return 1;
        }
        if (mod->loaded) {
            free(source);
            ast_free(prog);
            return 0; /* already loaded */
        }
    } else {
        mod = module_ensure(vm, peice_name);
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
                if (module_load(vm, stmt->as.bring_stmt.path, mod_dir)) {
                    *slash = '/';
                    mod->loading = 0;
                    free(source);
                    ast_free(prog);
                    return 1;
                }
                *slash = '/';
            } else {
                if (module_load(vm, stmt->as.bring_stmt.path, base_dir)) {
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
                    strcmp(other->as.var_decl.name, ship_name) == 0) {
                    is_job = 0;
                    break;
                }
            }
            ModMemberKind kind = is_job ? MOD_MEMBER_JOB : MOD_MEMBER_VAR;
            ModMember *mm = module_member_add(mod, ship_name, kind);
            mm->is_exported = 1;
        }
    }

    mod->loaded = 1;
    mod->loading = 0;
    free(source);
    /* Do NOT ast_free(prog) - we need the PEICE AST for compilation later */
    return 0;
}

static void module_compile_all(Compiler *c, VM *vm) {
    for (int m = 0; m < vm->module_count; m++) {
        Module *mod = &vm->modules[m];
        if (!mod->loaded) continue;
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s#", mod->name);
        const char *saved_prefix = c->module_prefix;
        c->module_prefix = prefix;
        
        /* Only compile JOB_DECL statements from the module body.
           Top-level statements (HARD, VAR, SHIP) are executed during module_initialize. */
        ASTNode *body = mod->peice_ast->as.peice_decl.body;
        if (body && body->type == NODE_BLOCK) {
            /* Create a temporary block with only JOB_DECL statements */
            ASTNode *job_block = ast_new(NODE_BLOCK, 0);
            nodelist_init(&job_block->as.block.statements);
            for (int i = 0; i < body->as.block.statements.count; i++) {
                ASTNode *stmt = body->as.block.statements.items[i];
                if (stmt && stmt->type == NODE_JOB_DECL) {
                    nodelist_push(&job_block->as.block.statements, stmt);
                }
            }
            compile_block(c, job_block);
            /* Free the temporary block structure but not the statements */
            free(job_block->as.block.statements.items);
            free(job_block);
        }
        
        c->module_prefix = saved_prefix;
    }
}

static void module_finalize_jobs(VM *vm, Module *mod) {
    for (int i = 0; i < mod->member_count; i++) {
        ModMember *mm = &mod->members[i];
        if (mm->kind == MOD_MEMBER_JOB && mm->is_exported) {
            /* The job was registered with prefix modname#jobname.
               Find it and record the internal name. */
            char internal[128];
            snprintf(internal, sizeof(internal), "%s#%s", mod->name, mm->name);
            for (int j = 0; j < vm->job_count; j++) {
                if (strcmp(vm->jobs[j].name, internal) == 0) {
                    mm->job_internal = strdup(internal);
                    break;
                }
            }
        }
    }
}

/* Initialize a module: run its top-level statements to allocate SHIPped variables.
   Returns 0 on success, 1 on error. */
static int module_initialize(VM *vm, Module *mod) {
    if (!mod->loaded || mod->peice_ast == NULL) return 0;
    
    /* Push a persistent scope for this module */
    Scope *outer = vm->scope;
    vm->scope = scope_push(outer);
    mod->persistent_scope = vm->scope;
    vm->active_module = mod - vm->modules;
    vm->initializing_module = vm->active_module;
    
    /* Run the module's top-level statements (not JOB bodies, which are skipped via JUMP).
       The module's PEICE body is an ASTNode block. We'll execute each statement
       using the existing exec_simple_stmt function. */
    ASTNode *body = mod->peice_ast->as.peice_decl.body;
    if (body && body->type == NODE_BLOCK) {
        for (int i = 0; i < body->as.block.statements.count; i++) {
            ASTNode *stmt = body->as.block.statements.items[i];
            if (!stmt) continue;
            /* Skip JOB_DECL - these are handled separately via job registration */
            if (stmt->type == NODE_JOB_DECL) continue;
            /* Skip SHIP_STMT - already processed at load time */
            if (stmt->type == NODE_SHIP_STMT) continue;
            /* Skip BRING_STMT - nested modules already loaded */
            if (stmt->type == NODE_BRING_STMT) continue;
            /* Execute the statement */
            exec_simple_stmt(vm, stmt);
        }
    }
    
    /* After running, scan the module's persistent scope for SHIPped variables
       and record their VMA slots */
    for (int i = 0; i < mod->member_count; i++) {
        ModMember *mm = &mod->members[i];
        if (mm->kind == MOD_MEMBER_VAR && mm->is_exported) {
            Symbol *sym = scope_find_local(mod->persistent_scope, mm->name);
            if (sym && sym->vma[0]) {
                RVmaSlot *slot = rvma_lookup(&vm->vmas, sym->vma);
                if (slot && slot->allocated) {
                    mm->slot = slot;
                }
            }
        }
    }
    
    vm->active_module = -1;
    vm->initializing_module = -1;
    vm->scope = outer;
    return 0;
}
    static void runtime_error(VM *vm, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[line %d] Runtime error: ", line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    vm->had_runtime_error = 1;
    longjmp(vm->abort_buf, 1);
}

static void enqueue(VM *vm, int handler_index, int has_values, Value old_v, Value new_v) {
    if (vm->q_count == vm->q_capacity) {
        vm->q_capacity = vm->q_capacity ? vm->q_capacity * 2 : 16;
        vm->queue = realloc(vm->queue, sizeof(QueueEntry) * vm->q_capacity);
    }
    QueueEntry *e = &vm->queue[vm->q_count++];
    e->handler_index = handler_index;
    e->has_values = has_values;
    e->old_value = old_v;
    e->new_value = new_v;
}

static void scope_pop_with_autoclean(VM *vm) {
    Scope *s = vm->scope;
    for (int i = 0; i < s->count; i++) {
        Symbol *sym = &s->symbols[i];
        if (!sym->is_loop_var && sym->autocleans && sym->vma[0]) rvma_free(&vm->vmas, sym->vma);
        if (sym->is_loop_var) value_free(&sym->direct_value);
    }
    vm->scope = s->parent;
    for (int i = 0; i < s->count; i++) free(s->symbols[i].name);
    free(s->symbols);
    free(s);
}

/* =======================================================================
 * Expression evaluation
 * ===================================================================== */
static Value eval_expr(VM *vm, ASTNode *node);

static Value *lvalue_ptr(VM *vm, ASTNode *node, int line) {
    if (node->type == NODE_INDEX) {
        Value *arr = lvalue_ptr(vm, node->as.index_expr.array, line);
        if (!arr) return NULL;
        Value idxv = eval_expr(vm, node->as.index_expr.index);
        long i = value_as_long(idxv);
        value_free(&idxv);
        if (arr->type != VAL_COLL) runtime_error(vm, line, "cannot index a non-collection value");
        if (i < 0 || i >= arr->count) runtime_error(vm, line, "index %ld out of bounds (collection has %d element(s))", i, arr->count);
        return &arr->items[i];
    }
    if (node->type == NODE_VMA_REF) {
        RVmaSlot *slot = rvma_lookup(&vm->vmas, node->as.vma_ref.name);
        if (!slot || !slot->allocated) runtime_error(vm, line, "VMA %s is not allocated", node->as.vma_ref.name);
        return &slot->value;
    }
    if (node->type == NODE_IDENTIFIER) {
        const char *name = node->as.identifier.name;
        if (strcmp(name, "OLD_VALUE") == 0) return &vm->cur_old_value;
        if (strcmp(name, "NEW_VALUE") == 0) return &vm->cur_new_value;
        Symbol *sym = scope_resolve(vm->scope, name);
        if (!sym) runtime_error(vm, line, "undeclared identifier '%s'", name);
        if (sym->is_loop_var) return &sym->direct_value;
        RVmaSlot *slot = rvma_lookup(&vm->vmas, sym->vma);
        if (!slot || !slot->allocated) runtime_error(vm, line, "VMA %s is not allocated", sym->vma);
        return &slot->value;
    }
    return NULL;
}

static const char *target_vma_addr(VM *vm, ASTNode *node) {
    if (node->type == NODE_VMA_REF) return node->as.vma_ref.name;
    if (node->type == NODE_IDENTIFIER) {
        const char *name = node->as.identifier.name;
        if (strcmp(name, "OLD_VALUE") == 0 || strcmp(name, "NEW_VALUE") == 0) return NULL;
        Symbol *sym = scope_resolve(vm->scope, name);
        if (sym && !sym->is_loop_var) return sym->vma;
    }
    return NULL;
}

static void dispatch_triggers(VM *vm, const char *addr, Value old_v, int changed) {
    for (int i = 0; i < vm->handler_count; i++) {
        Handler *h = &vm->handlers[i];
        if (h->kind == WHEN_VMA_CHANGED) {
            if (strcmp(h->vma_addr, addr) == 0 && changed) {
                RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
                enqueue(vm, i, 1, value_copy(old_v), value_copy(slot->value));
            }
        } else if (h->kind == WHEN_CONDITION) {
            Value cv = eval_expr(vm, h->condition);
            int cur = value_to_bool(cv);
            value_free(&cv);
            if (!h->last_state && cur) { enqueue(vm, i, 0, value_emp(), value_emp()); h->last_state = 1; }
            else if (h->last_state && !cur) { h->last_state = 0; }
        }
    }
}

static void rvma_set(VM *vm, const char *addr, Value new_val, int line) {
    RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
    if (!slot || !slot->allocated) { value_free(&new_val); runtime_error(vm, line, "VMA %s is not allocated", addr); }
    Value old_copy = value_copy(slot->value);
    int changed = !value_equal(slot->value, new_val);
    value_free(&slot->value);
    slot->value = new_val;
    dispatch_triggers(vm, addr, old_copy, changed);
    value_free(&old_copy);
}

static Value binary_op(VM *vm, VOTokenType op, Value l, Value r, int line) {
    switch (op) {
        case TOKEN_PLUS:
            if (l.type == VAL_TEX || r.type == VAL_TEX) {
                char *ls = value_to_cstr(l), *rs = value_to_cstr(r);
                char *cat = malloc(strlen(ls) + strlen(rs) + 1);
                strcpy(cat, ls); strcat(cat, rs);
                Value out = value_tex(cat);
                free(ls); free(rs); free(cat);
                return out;
            }
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'+' needs numeric or text operands");
            return (l.type == VAL_DEC || r.type == VAL_DEC)
                ? value_dec(value_as_double(l) + value_as_double(r))
                : value_num(l.num + r.num);
        case TOKEN_MINUS:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'-' needs numeric operands");
            return (l.type == VAL_DEC || r.type == VAL_DEC)
                ? value_dec(value_as_double(l) - value_as_double(r))
                : value_num(l.num - r.num);
        case TOKEN_STAR:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'*' needs numeric operands");
            return (l.type == VAL_DEC || r.type == VAL_DEC)
                ? value_dec(value_as_double(l) * value_as_double(r))
                : value_num(l.num * r.num);
        case TOKEN_SLASH:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'/' needs numeric operands");
            if (value_as_double(r) == 0.0) runtime_error(vm, line, "division by zero");
            return (l.type == VAL_DEC || r.type == VAL_DEC)
                ? value_dec(value_as_double(l) / value_as_double(r))
                : value_num(l.num / r.num);
        case TOKEN_PERCENT:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'%%' needs numeric operands");
            if (l.type == VAL_DEC || r.type == VAL_DEC) {
                if (value_as_double(r) == 0.0) runtime_error(vm, line, "division by zero");
                return value_dec(fmod(value_as_double(l), value_as_double(r)));
            }
            if (r.num == 0) runtime_error(vm, line, "division by zero");
            return value_num(l.num % r.num);
        case TOKEN_POWER: {
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'**' needs numeric operands");
            double result = pow(value_as_double(l), value_as_double(r));
            if (l.type == VAL_NUM && r.type == VAL_NUM && r.num >= 0) return value_num((long)llround(result));
            return value_dec(result);
        }
        case TOKEN_SHL:
            if (l.type != VAL_NUM || r.type != VAL_NUM) runtime_error(vm, line, "'<<' needs NUM operands");
            return value_num(l.num << r.num);
        case TOKEN_SHR:
            if (l.type != VAL_NUM || r.type != VAL_NUM) runtime_error(vm, line, "'>>' needs NUM operands");
            return value_num(l.num >> r.num);
        case TOKEN_AMP:
            if (l.type != VAL_NUM || r.type != VAL_NUM) runtime_error(vm, line, "'&' needs NUM operands");
            return value_num(l.num & r.num);
        case TOKEN_CARET:
            if (l.type != VAL_NUM || r.type != VAL_NUM) runtime_error(vm, line, "'^' needs NUM operands");
            return value_num(l.num ^ r.num);
        case TOKEN_PIPE:
            if (l.type != VAL_NUM || r.type != VAL_NUM) runtime_error(vm, line, "'|' needs NUM operands");
            return value_num(l.num | r.num);
        case TOKEN_EQEQ: return value_yn(value_equal(l, r));
        case TOKEN_NEQ:  return value_yn(!value_equal(l, r));
        case TOKEN_LT:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'<' needs numeric operands");
            return value_yn(value_as_double(l) < value_as_double(r));
        case TOKEN_GT:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'>' needs numeric operands");
            return value_yn(value_as_double(l) > value_as_double(r));
        case TOKEN_LE:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'<=' needs numeric operands");
            return value_yn(value_as_double(l) <= value_as_double(r));
        case TOKEN_GE:
            if (!value_is_numeric(l) || !value_is_numeric(r)) runtime_error(vm, line, "'>=' needs numeric operands");
            return value_yn(value_as_double(l) >= value_as_double(r));
        case TOKEN_XOR: return value_yn(value_to_bool(l) ^ value_to_bool(r));
        default: runtime_error(vm, line, "unsupported operator");
    }
    return value_emp();
}

static Value apply_assign_op(VM *vm, VOTokenType op, Value current, Value rhs, int line) {
    if (op == TOKEN_ASSIGN) return value_copy(rhs);
    if (current.type == VAL_COLL && op == TOKEN_PLUS_ASSIGN) {
        Value out = value_copy(current);
        value_coll_push(&out, value_copy(rhs));
        return out;
    }
    VOTokenType base;
    switch (op) {
        case TOKEN_PLUS_ASSIGN: base = TOKEN_PLUS; break;
        case TOKEN_MINUS_ASSIGN: base = TOKEN_MINUS; break;
        case TOKEN_STAR_ASSIGN: base = TOKEN_STAR; break;
        case TOKEN_SLASH_ASSIGN: base = TOKEN_SLASH; break;
        case TOKEN_PERCENT_ASSIGN: base = TOKEN_PERCENT; break;
        default: runtime_error(vm, line, "unsupported assignment operator"); return value_emp();
    }
    return binary_op(vm, base, current, rhs, line);
}

/* Forward declaration */
static void run_range(VM *vm, int start);

/* Module member resolution helpers */
static ModMember *module_resolve_member(VM *vm, const char *mod_name, const char *member_name) {
    Module *mod = module_find(vm, mod_name);
    if (!mod || !mod->loaded) return NULL;
    return module_member_find(mod, member_name);
}

static JobEntry *job_find_by_name(VM *vm, const char *name) {
    for (int i = 0; i < vm->job_count; i++)
        if (strcmp(vm->jobs[i].name, name) == 0)
            return &vm->jobs[i];
    return NULL;
}

static Value eval_expr(VM *vm, ASTNode *node) {
    if (!node) return value_emp();
    switch (node->type) {
        case NODE_NUM_LITERAL: return value_num(node->as.num_lit.value);
        case NODE_DEC_LITERAL: return value_dec(node->as.dec_lit.value);
        case NODE_TEX_LITERAL: return value_tex(node->as.tex_lit.value);
        case NODE_BOOL_LITERAL: return value_yn(node->as.bool_lit.value);
        case NODE_EMP_LITERAL: return value_emp();

        case NODE_VMA_REF: {
            RVmaSlot *slot = rvma_lookup(&vm->vmas, node->as.vma_ref.name);
            if (!slot || !slot->allocated) runtime_error(vm, node->line, "VMA %s is not allocated", node->as.vma_ref.name);
            return value_copy(slot->value);
        }

        case NODE_IDENTIFIER: {
            const char *name = node->as.identifier.name;
            if (strcmp(name, "OLD_VALUE") == 0) return value_copy(vm->cur_old_value);
            if (strcmp(name, "NEW_VALUE") == 0) return value_copy(vm->cur_new_value);
            Symbol *sym = scope_resolve(vm->scope, name);
            if (!sym) runtime_error(vm, node->line, "undeclared identifier '%s'", name);
            if (sym->is_loop_var) return value_copy(sym->direct_value);
            RVmaSlot *slot = rvma_lookup(&vm->vmas, sym->vma);
            if (!slot || !slot->allocated) runtime_error(vm, node->line, "VMA %s is not allocated", sym->vma);
            return value_copy(slot->value);
        }

        case NODE_UNARY: {
            if (node->as.unary.op == TOKEN_NOT) {
                Value v = eval_expr(vm, node->as.unary.operand);
                int b = value_to_bool(v);
                value_free(&v);
                return value_yn(!b);
            }
            Value v = eval_expr(vm, node->as.unary.operand);
            if (v.type == VAL_DEC) { double d = -v.dec; return value_dec(d); }
            if (v.type == VAL_NUM) { long n = -v.num; return value_num(n); }
            runtime_error(vm, node->line, "unary '-' needs a numeric operand");
            return value_emp();
        }

        case NODE_BINARY: {
            if (node->as.binary.op == TOKEN_AND) {
                Value l = eval_expr(vm, node->as.binary.left);
                if (!value_to_bool(l)) { value_free(&l); return value_yn(0); }
                value_free(&l);
                Value r = eval_expr(vm, node->as.binary.right);
                int b = value_to_bool(r);
                value_free(&r);
                return value_yn(b);
            }
            if (node->as.binary.op == TOKEN_OR) {
                Value l = eval_expr(vm, node->as.binary.left);
                if (value_to_bool(l)) { value_free(&l); return value_yn(1); }
                value_free(&l);
                Value r = eval_expr(vm, node->as.binary.right);
                int b = value_to_bool(r);
                value_free(&r);
                return value_yn(b);
            }
            Value l = eval_expr(vm, node->as.binary.left);
            Value r = eval_expr(vm, node->as.binary.right);
            Value out = binary_op(vm, node->as.binary.op, l, r, node->line);
            value_free(&l); value_free(&r);
            return out;
        }

        case NODE_ASSIGN: {
            Value rhs = eval_expr(vm, node->as.assign.value);
            Value result_copy;
            const char *addr = target_vma_addr(vm, node->as.assign.target);
            if (addr) {
                RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
                if (!slot || !slot->allocated) { value_free(&rhs); runtime_error(vm, node->line, "VMA %s is not allocated", addr); }
                Value newval = apply_assign_op(vm, node->as.assign.op, slot->value, rhs, node->line);
                value_free(&rhs);
                result_copy = value_copy(newval);
                rvma_set(vm, addr, newval, node->line);
            } else {
                Value *ptr = lvalue_ptr(vm, node->as.assign.target, node->line);
                Value newval = apply_assign_op(vm, node->as.assign.op, *ptr, rhs, node->line);
                value_free(&rhs);
                value_free(ptr);
                *ptr = newval;
                result_copy = value_copy(newval);
            }
            return result_copy;
        }

        case NODE_LOAD:
            return eval_expr(vm, node->as.load.vma);

        case NODE_LENGTH_CALL: {
            Value v = eval_expr(vm, node->as.length_call.arg);
            long len;
            if (v.type == VAL_TEX) len = (long)strlen(v.tex ? v.tex : "");
            else if (v.type == VAL_COLL) len = v.count;
            else { value_free(&v); runtime_error(vm, node->line, "LENGTH() needs a TEX or COLL argument"); return value_emp(); }
            value_free(&v);
            return value_num(len);
        }

        case NODE_ARRAY_LITERAL: {
            Value out = value_coll_empty();
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                value_coll_push(&out, eval_expr(vm, node->as.array_lit.elements.items[i]));
            return out;
        }

        case NODE_INDEX: {
            Value arr = eval_expr(vm, node->as.index_expr.array);
            Value idxv = eval_expr(vm, node->as.index_expr.index);
            long i = value_as_long(idxv);
            value_free(&idxv);
            if (arr.type != VAL_COLL) { value_free(&arr); runtime_error(vm, node->line, "cannot index a non-collection value"); }
            if (i < 0 || i >= arr.count) { value_free(&arr); runtime_error(vm, node->line, "index %ld out of bounds", i); }
            Value out = value_copy(arr.items[i]);
            value_free(&arr);
            return out;
        }

        case NODE_MODULE_REF: {
            /* module.member value reference - resolve to module member variable */
            const char *mod_name = node->as.module_ref.module;
            const char *member_name = node->as.module_ref.member;
            ModMember *mm = module_resolve_member(vm, mod_name, member_name);
            if (!mm) runtime_error(vm, node->line, "module '%s' has no member '%s'", mod_name, member_name);
            if (!mm->is_exported) runtime_error(vm, node->line, "module '%s' member '%s' is not exported", mod_name, member_name);
            if (mm->kind == MOD_MEMBER_VAR) {
                if (!mm->slot || !mm->slot->allocated) runtime_error(vm, node->line, "module member '%s.%s' is not allocated", mod_name, member_name);
                return value_copy(mm->slot->value);
            } else {
                runtime_error(vm, node->line, "module '%s' member '%s' is a job, not a value", mod_name, member_name);
            }
            return value_emp();
        }

        case NODE_CALL: {
            JobEntry *job = NULL;
            int saved_active_module = vm->active_module;
            int call_active_module = -1;

            if (node->as.call.callee->type == NODE_MODULE_REF) {
                /* Module-member call: math.ADD(...) */
                const char *mod_name = node->as.call.callee->as.module_ref.module;
                const char *member_name = node->as.call.callee->as.module_ref.member;
                ModMember *mm = module_resolve_member(vm, mod_name, member_name);
                if (!mm) runtime_error(vm, node->line, "module '%s' has no member '%s'", mod_name, member_name);
                if (!mm->is_exported) runtime_error(vm, node->line, "module '%s' member '%s' is not exported", mod_name, member_name);
                if (mm->kind != MOD_MEMBER_JOB) runtime_error(vm, node->line, "module '%s' member '%s' is not a job", mod_name, member_name);
                if (!mm->job_internal) runtime_error(vm, node->line, "module '%s' job '%s' not found (internal error)", mod_name, member_name);
                job = job_find_by_name(vm, mm->job_internal);
                if (!job) runtime_error(vm, node->line, "internal job '%s' not found", mm->job_internal);
                call_active_module = module_find(vm, mod_name) - vm->modules;
            } else if (node->as.call.callee->type == NODE_IDENTIFIER) {
                /* Direct call - could be a bare call to a sibling job inside
                   the same module (SHIPped or not - SHIP only controls
                   external/cross-module visibility, not intra-module
                   callability). Try the current module's own jobs first via
                   their "module#job" registered name, then fall back to a
                   top-level job of the same name. */
                const char *job_name = node->as.call.callee->as.identifier.name;
                job = NULL;
                if (vm->active_module >= 0) {
                    Module *mod = &vm->modules[vm->active_module];
                    char qualified[128];
                    snprintf(qualified, sizeof(qualified), "%s#%s", mod->name, job_name);
                    job = job_find_by_name(vm, qualified);
                    if (job) call_active_module = vm->active_module;
                }
                if (!job) job = job_find_by_name(vm, job_name);
                if (!job) runtime_error(vm, node->line, "undefined job '%s'", job_name);
            } else {
                runtime_error(vm, node->line, "only direct function calls are supported");
            }

            /* Evaluate arguments */
            int nargs = node->as.call.args.count;
            if (nargs != job->param_count)
                runtime_error(vm, node->line, "job '%s' expects %d arguments but got %d",
                              job->name, job->param_count, nargs);

            Value *args = nargs ? malloc(sizeof(Value) * nargs) : NULL;
            for (int i = 0; i < nargs; i++)
                args[i] = eval_expr(vm, node->as.call.args.items[i]);

            /* Save VM state */
            Scope *saved_scope = vm->scope;
            int saved_giving = vm->giving;
            Value saved_give = vm->give_value;
            vm->giving = 0;

            /* Push scope, bind params as VMA-backed variables */
            vm->scope = scope_push(saved_scope);
            for (int i = 0; i < nargs; i++) {
                RVmaSlot *slot = rvma_alloc_next(&vm->vmas, job->params[i].name);
                Symbol *sym = scope_declare(vm->scope, job->params[i].name);
                snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
                value_free(&slot->value);
                slot->value = args[i];
            }

            /* Set active module for this job execution */
            vm->active_module = call_active_module;

            /* Run the job body */
            run_range(vm, job->body_start);

            /* Get result */
            Value result = vm->giving ? vm->give_value : value_emp();

            /* Restore state */
            vm->give_value = saved_give;
            vm->giving = saved_giving;
            vm->active_module = saved_active_module;

            /* Pop scope (frees param VMAs via AUTOCLEAN if set) */
            scope_pop_with_autoclean(vm);

            /* Free args array (values are now in VMAs or copied) */
            free(args);

            return result;
        }

        default:
            return value_emp();
    }
}

/* =======================================================================
 * Statement execution
 * ===================================================================== */
static Value default_value_for_type(VOTokenType t) {
    switch (t) {
        case TOKEN_TYPE_NUM: return value_num(0);
        case TOKEN_TYPE_DEC: return value_dec(0.0);
        case TOKEN_TYPE_TEX: return value_tex("");
        case TOKEN_TYPE_YN:  return value_yn(0);
        case TOKEN_TYPE_COLL: return value_coll_empty();
        case TOKEN_TYPE_EMP: return value_emp();
        default: return value_emp();
    }
}

static void exec_var_decl(VM *vm, ASTNode *node) {
    const char *name = node->as.var_decl.name;
    ASTNode *init = node->as.var_decl.init;
    int is_aliasing_form = init && init->type == NODE_VMA_REF;

    if (scope_find_local(vm->scope, name))
        runtime_error(vm, node->line, "'%s' is already declared in this scope", name);

    RVmaSlot *slot;
    if (is_aliasing_form) {
        const char *addr = init->as.vma_ref.name;
        slot = rvma_slot_for_index(&vm->vmas, vma_canonical_index(addr));
        if (slot->allocated) runtime_error(vm, node->line, "cannot bind '%s' to %s - already allocated (aliasing is banned)", name, addr);
        slot->allocated = 1;
        free(slot->owner);
        slot->owner = strdup(name);
        slot->value = default_value_for_type(node->as.var_decl.var_type);
    } else {
        Value initval = eval_expr(vm, init);
        slot = rvma_alloc_next(&vm->vmas, name);
        slot->value = initval;
    }

    Symbol *sym = scope_declare(vm->scope, name);
    sym->autocleans = vm->autoclean_on;
    snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
}

static void exec_hard_decl(VM *vm, ASTNode *node) {
    const char *name = node->as.hard_decl.name;
    ASTNode *value_expr = node->as.hard_decl.value;

    if (scope_find_local(vm->scope, name))
        runtime_error(vm, node->line, "'%s' is already declared in this scope", name);

    Value val = eval_expr(vm, value_expr);
    RVmaSlot *slot = rvma_alloc_next(&vm->vmas, name);
    slot->value = val;

    Symbol *sym = scope_declare(vm->scope, name);
    sym->is_const = 1;
    sym->autocleans = vm->autoclean_on;
    snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
}

static void exec_clean(VM *vm, ASTNode *node) {
    const char *target = node->as.clean_stmt.target;
    if (is_vma_format(target)) {
        rvma_free(&vm->vmas, target);
        return;
    }
    Symbol *sym = scope_resolve(vm->scope, target);
    if (!sym) runtime_error(vm, node->line, "undeclared identifier '%s'", target);
    if (!sym->is_loop_var && sym->vma[0]) rvma_free(&vm->vmas, sym->vma);
}

static void do_assign(VM *vm, ASTNode *target, VOTokenType op, Value rhs, int line) {
    const char *addr = target_vma_addr(vm, target);
    if (addr) {
        RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
        if (!slot || !slot->allocated) { value_free(&rhs); runtime_error(vm, line, "VMA %s is not allocated", addr); }
        Value newval = apply_assign_op(vm, op, slot->value, rhs, line);
        value_free(&rhs);
        rvma_set(vm, addr, newval, line);
    } else {
        Value *ptr = lvalue_ptr(vm, target, line);
        Value newval = apply_assign_op(vm, op, *ptr, rhs, line);
        value_free(&rhs);
        value_free(ptr);
        *ptr = newval;
    }
}

static void exec_simple_stmt(VM *vm, ASTNode *node) {
    switch (node->type) {
        case NODE_VAR_DECL:
            exec_var_decl(vm, node);
            break;
        case NODE_HARD_DECL:
            exec_hard_decl(vm, node);
            break;
        case NODE_EXPR_STMT: {
            Value v = eval_expr(vm, node->as.expr_stmt.expr);
            value_free(&v);
            break;
        }
        case NODE_INC_DEC_STMT: {
            VOTokenType op = node->as.inc_dec.op == TOKEN_INCREMENT ? TOKEN_PLUS_ASSIGN : TOKEN_MINUS_ASSIGN;
            do_assign(vm, node->as.inc_dec.target, op, value_num(1), node->line);
            break;
        }
        case NODE_SHOW_STMT: {
            Value v = eval_expr(vm, node->as.show_stmt.expr);
            char *s = value_to_cstr(v);
            printf("%s\n", s);
            free(s);
            value_free(&v);
            break;
        }
        case NODE_STORE_STMT: {
            Value v = eval_expr(vm, node->as.store_stmt.value);
            rvma_set(vm, node->as.store_stmt.target_vma, v, node->line);
            break;
        }
        case NODE_CLEAN_STMT:
            exec_clean(vm, node);
            break;
        case NODE_CLEANALL_STMT:
            rvma_free_all(&vm->vmas);
            break;
        case NODE_AUTOCLEAN_STMT:
            vm->autoclean_on = node->as.autoclean_stmt.on;
            break;
        case NODE_DEMAND_STMT: {
            Value cond = eval_expr(vm, node->as.demand_stmt.condition);
            int b = value_to_bool(cond);
            value_free(&cond);
            if (!b) {
                if (node->as.demand_stmt.message)
                    runtime_error(vm, node->line, "DEMAND failed: %s", node->as.demand_stmt.message);
                else
                    runtime_error(vm, node->line, "DEMAND condition not met");
            }
            break;
        }
        case NODE_SERVE_STMT: {
            Value v = eval_expr(vm, node->as.serve_stmt.value);
            char *msg = value_to_cstr(v);
            value_free(&v);
            if (vm->do_depth > 0) {
                /* Jump to the GRABE handler */
                free(msg);
                longjmp(vm->do_catch_buf, 1);
            }
            runtime_error(vm, node->line, "SERVE: %s", msg);
            free(msg);
            break;
        }
        case NODE_DO_STMT: {
            int saved_depth = vm->do_depth;
            vm->do_depth++;
            if (setjmp(vm->do_catch_buf)) {
                /* Caught an issue - run the catch block */
                vm->do_depth = saved_depth;
                /* We need to execute the catch block statements. Since we have the AST,
                   walk it directly. */
                vm->scope = scope_push(vm->scope);
                {
                    ASTNode *catch = node->as.do_stmt.catch_block;
                    if (catch) {
                        for (int i = 0; i < catch->as.block.statements.count; i++) {
                            ASTNode *stmt = catch->as.block.statements.items[i];
                            if (!stmt) continue;
                            /* Execute each statement directly */
                            switch (stmt->type) {
                                case NODE_SHOW_STMT: {
                                    Value v = eval_expr(vm, stmt->as.show_stmt.expr);
                                    char *s = value_to_cstr(v);
                                    printf("%s\n", s);
                                    free(s);
                                    value_free(&v);
                                    break;
                                }
                                case NODE_EXPR_STMT: {
                                    Value v = eval_expr(vm, stmt->as.expr_stmt.expr);
                                    value_free(&v);
                                    break;
                                }
                                case NODE_VAR_DECL:
                                    exec_var_decl(vm, stmt);
                                    break;
                                case NODE_ASSIGN: {
                                    Value rhs = eval_expr(vm, stmt->as.assign.value);
                                    const char *addr = target_vma_addr(vm, stmt->as.assign.target);
                                    if (addr) {
                                        RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
                                        if (slot && slot->allocated) {
                                            Value newval = apply_assign_op(vm, stmt->as.assign.op, slot->value, rhs, stmt->line);
                                            value_free(&rhs);
                                            rvma_set(vm, addr, newval, stmt->line);
                                        } else { value_free(&rhs); }
                                    } else {
                                        Value *ptr = lvalue_ptr(vm, stmt->as.assign.target, stmt->line);
                                        if (ptr) {
                                            Value newval = apply_assign_op(vm, stmt->as.assign.op, *ptr, rhs, stmt->line);
                                            value_free(&rhs);
                                            value_free(ptr);
                                            *ptr = newval;
                                        } else { value_free(&rhs); }
                                    }
                                    break;
                                }
                                default: {
                                    Value v = eval_expr(vm, stmt);
                                    value_free(&v);
                                    break;
                                }
                            }
                        }
                    }
                }
                scope_pop_with_autoclean(vm);
                vm->do_depth = saved_depth;
                return;
            }
            /* Try block - execute statements directly */
            vm->scope = scope_push(vm->scope);
            {
                ASTNode *try_block = node->as.do_stmt.try_block;
                if (try_block) {
                    for (int i = 0; i < try_block->as.block.statements.count; i++) {
                        ASTNode *stmt = try_block->as.block.statements.items[i];
                        if (!stmt) continue;
                        switch (stmt->type) {
                            case NODE_SHOW_STMT: {
                                Value v = eval_expr(vm, stmt->as.show_stmt.expr);
                                char *s = value_to_cstr(v);
                                printf("%s\n", s);
                                free(s);
                                value_free(&v);
                                break;
                            }
                            case NODE_EXPR_STMT: {
                                Value v = eval_expr(vm, stmt->as.expr_stmt.expr);
                                value_free(&v);
                                break;
                            }
                            case NODE_VAR_DECL:
                                exec_var_decl(vm, stmt);
                                break;
                            case NODE_SERVE_STMT: {
                                /* SERVE inside DO - longjmp to catch */
                                longjmp(vm->do_catch_buf, 1);
                                break;
                            }
                            case NODE_DEMAND_STMT: {
                                /* DEMAND inside DO - if the condition fails, route
                                   control to the GRABE handler instead of aborting */
                                Value cond = eval_expr(vm, stmt->as.demand_stmt.condition);
                                int b = value_to_bool(cond);
                                value_free(&cond);
                                if (!b) longjmp(vm->do_catch_buf, 1);
                                break;
                            }
                            case NODE_ASSIGN: {
                                Value rhs = eval_expr(vm, stmt->as.assign.value);
                                const char *addr = target_vma_addr(vm, stmt->as.assign.target);
                                if (addr) {
                                    RVmaSlot *slot = rvma_lookup(&vm->vmas, addr);
                                    if (slot && slot->allocated) {
                                        Value newval = apply_assign_op(vm, stmt->as.assign.op, slot->value, rhs, stmt->line);
                                        value_free(&rhs);
                                        rvma_set(vm, addr, newval, stmt->line);
                                    } else { value_free(&rhs); }
                                } else {
                                    Value *ptr = lvalue_ptr(vm, stmt->as.assign.target, stmt->line);
                                    if (ptr) {
                                        Value newval = apply_assign_op(vm, stmt->as.assign.op, *ptr, rhs, stmt->line);
                                        value_free(&rhs);
                                        value_free(ptr);
                                        *ptr = newval;
                                    } else { value_free(&rhs); }
                                }
                                break;
                            }
                            default: {
                                Value v = eval_expr(vm, stmt);
                                value_free(&v);
                                break;
                            }
                        }
                    }
                }
            }
            scope_pop_with_autoclean(vm);
            vm->do_depth = saved_depth;
            return;
        }
        case NODE_BRING_STMT:
        case NODE_SHIP_STMT:
            /* Module system stub */
            break;
        default:
            break;
    }
}

static void exec_for_setup(VM *vm, ASTNode *node) {
    Value start = eval_expr(vm, node->as.for_stmt.start);
    Value end = eval_expr(vm, node->as.for_stmt.end);
    long end_l = value_as_long(end);
    value_free(&end);
    if (vm->for_end_top >= (int)(sizeof(vm->for_end_stack) / sizeof(vm->for_end_stack[0])))
        runtime_error(vm, node->line, "FOR loops nested too deeply");
    vm->for_end_stack[vm->for_end_top++] = end_l;
    Symbol *sym = scope_declare(vm->scope, node->as.for_stmt.iterator);
    sym->is_loop_var = 1;
    sym->direct_value = value_num(value_as_long(start));
    value_free(&start);
}

static int exec_for_test(VM *vm, ASTNode *node) {
    Symbol *sym = scope_resolve(vm->scope, node->as.for_stmt.iterator);
    long end_l = vm->for_end_stack[vm->for_end_top - 1];
    return sym->direct_value.num <= end_l;
}

static void exec_for_step(VM *vm, ASTNode *node) {
    Symbol *sym = scope_resolve(vm->scope, node->as.for_stmt.iterator);
    sym->direct_value.num += 1;
}

static void exec_for_teardown(VM *vm, ASTNode *node) {
    (void)node;
    vm->for_end_top--;
    scope_pop_last(vm->scope);
}

static void register_handler(VM *vm, Instr *ins) {
    ASTNode *node = ins->node;
    if (vm->handler_count == vm->handler_capacity) {
        vm->handler_capacity = vm->handler_capacity ? vm->handler_capacity * 2 : 8;
        vm->handlers = realloc(vm->handlers, sizeof(Handler) * vm->handler_capacity);
    }
    Handler *h = &vm->handlers[vm->handler_count++];
    memset(h, 0, sizeof(*h));
    h->kind = node->as.when_stmt.kind;
    if (h->kind == WHEN_VMA_CHANGED)
        snprintf(h->vma_addr, sizeof(h->vma_addr), "%s", node->as.when_stmt.vma_name);
    else
        h->condition = node->as.when_stmt.condition;
    h->body_start = ins->target2;
}

static int step(VM *vm, int pc) {
    Instr *ins = &vm->code[pc];
    switch (ins->kind) {
        case I_STMT: exec_simple_stmt(vm, ins->node); return pc + 1;
        case I_JUMP: return ins->target;
        case I_JUMP_IF_FALSE: {
            Value v = eval_expr(vm, ins->node);
            int b = value_to_bool(v);
            value_free(&v);
            return b ? pc + 1 : ins->target;
        }
        case I_FOR_SETUP: exec_for_setup(vm, ins->node); return pc + 1;
        case I_FOR_TEST: return exec_for_test(vm, ins->node) ? pc + 1 : ins->target;
        case I_FOR_STEP: exec_for_step(vm, ins->node); return pc + 1;
        case I_FOR_TEARDOWN: exec_for_teardown(vm, ins->node); return pc + 1;
        case I_SCOPE_PUSH: vm->scope = scope_push(vm->scope); return pc + 1;
        case I_SCOPE_POP: scope_pop_with_autoclean(vm); return pc + 1;
        case I_WHEN_REGISTER: register_handler(vm, ins); return pc + 1;
        case I_HANDLER_RETURN: return pc + 1;
        case I_GIVE: {
            /* Evaluate the expression and set the give value */
            ASTNode *give_node = ins->node;
            Value v = eval_expr(vm, give_node->as.give_stmt.value);
            vm->give_value = v;
            vm->giving = 1;
            return pc + 1;
        }
    }
    return pc + 1;
}

static void run_range(VM *vm, int start) {
    int pc = start;
    long guard = 0;
    while (pc >= 0 && pc < vm->code_count && vm->code[pc].kind != I_HANDLER_RETURN && !vm->giving) {
        pc = step(vm, pc);
        if (++guard > 2000000L)
            runtime_error(vm, vm->code[start].node ? vm->code[start].node->line : 0,
                          "handler execution exceeded its instruction budget");
    }
}

static void drain_queue(VM *vm) {
    int depth = 0;
    while (vm->q_count > 0) {
        depth++;
        if (depth > vm->max_queue_depth)
            runtime_error(vm, 0, "Event queue overflow: possible infinite trigger loop");

        QueueEntry e = vm->queue[0];
        memmove(vm->queue, vm->queue + 1, sizeof(QueueEntry) * (vm->q_count - 1));
        vm->q_count--;

        Handler *h = &vm->handlers[e.handler_index];
        if (e.has_values) {
            vm->in_changed_handler = 1;
            vm->cur_old_value = e.old_value;
            vm->cur_new_value = e.new_value;
        } else {
            vm->in_changed_handler = 0;
        }

        run_range(vm, h->body_start);

        if (e.has_values) {
            value_free(&vm->cur_old_value);
            value_free(&vm->cur_new_value);
        }
        vm->in_changed_handler = 0;
    }
}

/* =======================================================================
 * Public entry point
 * ===================================================================== */
int run_program(ASTNode *program, const char *source_path) {
    VM *vm = calloc(1, sizeof(VM));
    if (!vm) return 1;
    vm->max_queue_depth = DEFAULT_MAX_QUEUE_DEPTH;

    /* Extract source directory from path for BRING resolution */
    char source_dir[4096];
    strncpy(source_dir, source_path, sizeof(source_dir) - 1);
    source_dir[sizeof(source_dir) - 1] = '\0';
    char *slash = strrchr(source_dir, '/');
    if (slash) *slash = '\0';
    else if ((slash = strrchr(source_dir, '\\'))) *slash = '\0';
    else { source_dir[0] = '.'; source_dir[1] = '\0'; } /* no dir component: file is in cwd, not filesystem root */

    Compiler c;
    InstrList code;
    /* module_load() (called from inside compile_program, while resolving
       BRING) reports failures via runtime_error(), which longjmps rather
       than returning normally - so that failure has to be caught by a
       setjmp established *before* compile_program runs, not just handled
       via c.had_error afterward. compile_program() zeroes both `c` and
       `code` before doing anything else, so if we do land back here via
       longjmp, freeing their (NULL) fields below is always safe. */
    if (setjmp(vm->abort_buf)) {
        free(code.items);
        for (int i = 0; i < c.label_count; i++) free(c.labels[i].name);
        free(c.labels);
        for (int i = 0; i < c.pending_count; i++) free(c.pending[i].label);
        free(c.pending);
        for (int i = 0; i < c.job_count; i++) {
            free(c.jobs[i].name);
            for (int j = 0; j < c.jobs[i].param_count; j++) free(c.jobs[i].params[j].name);
            free(c.jobs[i].params);
        }
        free(c.jobs);
        free(vm);
        return 1;
    }
    compile_program(&c, vm, program, source_dir, &code);
    if (c.had_error) {
        free(code.items);
        for (int i = 0; i < c.label_count; i++) free(c.labels[i].name);
        free(c.labels);
        for (int i = 0; i < c.pending_count; i++) free(c.pending[i].label);
        free(c.pending);
        for (int i = 0; i < c.job_count; i++) {
            free(c.jobs[i].name);
            for (int j = 0; j < c.jobs[i].param_count; j++) free(c.jobs[i].params[j].name);
            free(c.jobs[i].params);
        }
        free(c.jobs);
        free(vm);
        return 1;
    }
    vm->code = code.items;
    vm->code_count = code.count;
    vm->jobs = c.jobs;
    vm->job_count = c.job_count;

    /* Finalize module jobs after compilation */
    for (int m = 0; m < vm->module_count; m++) {
        Module *mod = &vm->modules[m];
        if (mod->loaded) {
            module_finalize_jobs(vm, mod);
        }
    }

    /* Initialize modules: run their top-level statements to allocate SHIPped variables.
       This can itself call runtime_error() (e.g. a bad expression in a
       module-level HARD/VAR init) - re-arm the same jmp_buf right here so
       that also lands somewhere valid instead of jumping to whatever
       (by-then-stale) context the setjmp() above captured. */
    if (setjmp(vm->abort_buf)) {
        free(vm->code);
        free(vm);
        return 1;
    }
    for (int m = 0; m < vm->module_count; m++) {
        Module *mod = &vm->modules[m];
        if (mod->loaded) {
            module_initialize(vm, mod);
        }
    }

    vm->scope = scope_push(NULL);

    int pc = 0;
    while (pc >= 0 && pc < vm->code_count) {
        pc = step(vm, pc);
        drain_queue(vm);
    }
    scope_pop_with_autoclean(vm);

    /* Cleanup jobs */
    for (int i = 0; i < vm->job_count; i++) {
        free(vm->jobs[i].name);
        for (int j = 0; j < vm->jobs[i].param_count; j++) free(vm->jobs[i].params[j].name);
        free(vm->jobs[i].params);
    }
    free(vm->jobs);

    /* Cleanup modules */
    for (int m = 0; m < vm->module_count; m++) {
        Module *mod = &vm->modules[m];
        free(mod->name);
        for (int i = 0; i < mod->member_count; i++) {
            free(mod->members[i].name);
            if (mod->members[i].job_internal) free(mod->members[i].job_internal);
        }
        free(mod->members);
        if (mod->peice_ast) ast_free(mod->peice_ast);
    }
    free(vm->modules);

    int rc = vm->had_runtime_error ? 1 : 0;
    free(vm->code);
    free(vm);
    return rc;
}
