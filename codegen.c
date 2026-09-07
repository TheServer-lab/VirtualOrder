#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "codegen.h"
#include "vo_rt.h"
#include "memstream_compat.h"

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
typedef struct {
    FILE *body;          /* memstream: function body instructions        */
    char *body_buf; size_t body_len;
    FILE *rodata;         /* memstream: string literal constants          */
    char *rodata_buf; size_t rodata_len;
    CgTarget target;
    ABI abi;
    int depth, max_depth; /* expr temp-slot stack (LIFO)                   */
    int label_id;
    int str_id;
    int for_id;
    CScope *scope;
    CVmaTable vmas;
    GVma *globals; int gcount, gcap;
    int had_error;
    int autoclean_on;
} CG;

static void cg_error(CG *cg, int line, const char *msg) {
    fprintf(stderr, "[line %d] Compile error: %s\n", line, msg);
    cg->had_error = 1;
}

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
   is taken with lea; `line` args are plain immediates.                    */
static void emit_lea(CG *cg, const char *opnd, const char *reg) {
    fprintf(cg->body, "    leaq %s, %s\n", opnd, reg);
}
static void call0(CG *cg, const char *fn) { fprintf(cg->body, "    call %s\n", fn); }

static void callN(CG *cg, const char *fn, int n, const char *opnds[], int is_imm[]) {
    for (int i = 0; i < n; i++) {
        if (is_imm[i]) fprintf(cg->body, "    movq $%s, %s\n", opnds[i], cg->abi.argreg[i]);
        else emit_lea(cg, opnds[i], cg->abi.argreg[i]);
    }
    call0(cg, fn);
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

static void free_if_temp(CG *cg, Opnd *o) {
    if (o->is_temp) { call1(cg, "vo_free", o->text); free_temp(cg, o); }
}

static int new_label(CG *cg) { return cg->label_id++; }

/* forward decls */
static Opnd emit_expr(CG *cg, ASTNode *node);
static void emit_stmt(CG *cg, ASTNode *node);
static void emit_block(CG *cg, ASTNode *block);

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
        case NODE_NULL_LITERAL:
            out = alloc_temp(cg);
            call1(cg, "vo_set_null", out.text);
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
        case NODE_IDENTIFIER: {
            CSym *sym = scope_resolve(cg->scope, node->as.identifier.name);
            if (!sym) { cg_error(cg, node->line, "internal: unresolved identifier (analyzer should have caught this)"); out = alloc_temp(cg); return out; }
            return global_opnd(sym->is_loop_var ? sym->loop_label : global_label_for_addr(cg, sym->vma));
        }
        case NODE_VMA_REF:
            return global_opnd(global_label_for_addr(cg, node->as.vma_ref.name));
        case NODE_LOAD:
            return emit_expr(cg, node->as.load.vma);
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
            if (needs_line) call4_imm(cg, fn, l.text, l.text, r.text, node->line);
            else call3(cg, fn, l.text, l.text, r.text);
            free_if_temp(cg, &r);
            return l;
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
        case NODE_CONST_DECL:
            declare_var(cg, node);
            break;
        case NODE_EXPR_STMT: {
            Opnd v = emit_expr(cg, node->as.expr_stmt.expr);
            free_if_temp(cg, &v);
            break;
        }
        case NODE_SHOW_STMT: {
            Opnd v = emit_expr(cg, node->as.show_stmt.expr);
            call1(cg, "vo_show", v.text);
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
                    free_if_temp(cg, &c);
                    fprintf(cg->body, "    testl %%eax, %%eax\n");
                    fprintf(cg->body, "    jz .Lnext%d\n", next_lbl);
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
            free_if_temp(cg, &c);
            fprintf(cg->body, "    testl %%eax, %%eax\n");
            fprintf(cg->body, "    jz .Lend%d\n", end);
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
        default:
            cg_error(cg, node->line, "this statement form is not yet supported by the native compiler");
            break;
    }
}

static void emit_block(CG *cg, ASTNode *block) {
    for (int i = 0; i < block->as.block.statements.count; i++)
        emit_stmt(cg, block->as.block.statements.items[i]);
}

/* =========================================================================
 * Top level driver
 * ========================================================================= */
int codegen_compile(ASTNode *program, CgTarget target, FILE *out) {
    CG cg; memset(&cg, 0, sizeof(cg));
    cg.target = target;
    cg.abi = abi_for(target);
    cg.scope = scope_push(NULL);
    cg.body = open_memstream(&cg.body_buf, &cg.body_len);
    cg.rodata = open_memstream(&cg.rodata_buf, &cg.rodata_len);

    int saved_for_id_start = cg.for_id;
    emit_block(&cg, program);
    int total_for_loops = cg.for_id - saved_for_id_start;

    MEMSTREAM_CLOSE(cg.body, &cg.body_buf, &cg.body_len);
    MEMSTREAM_CLOSE(cg.rodata, &cg.rodata_buf, &cg.rodata_len);

    if (cg.had_error) { free(cg.body_buf); free(cg.rodata_buf); return 1; }

    long frame_bytes = (long)(cg.max_depth + 1) * SLOT_SIZE + cg.abi.shadow_space;
    frame_bytes = (frame_bytes + 15) & ~15L; /* round up to 16 for call-site alignment */

    fprintf(out, "# Generated by voc (Virtual Order native compiler). Do not edit by hand.\n");
    fprintf(out, "    .text\n");
    fprintf(out, "    .globl vo_main\n");
    fprintf(out, "vo_main:\n");
    fprintf(out, "    pushq %%rbp\n");
    fprintf(out, "    movq %%rsp, %%rbp\n");
    fprintf(out, "    subq $%ld, %%rsp\n", frame_bytes);
    fprintf(out, "%s", cg.body_buf);
    fprintf(out, "    leave\n");
    fprintf(out, "    ret\n\n");

    fprintf(out, "    .section .rodata\n");
    fprintf(out, "%s", cg.rodata_buf);
    fprintf(out, "\n");

    fprintf(out, "    .bss\n");
    for (int i = 0; i < cg.gcount; i++)
        fprintf(out, "    .align 8\n%s:\n    .zero %ld\n", cg.globals[i].label, SLOT_SIZE);
    for (int i = 0; i < total_for_loops; i++) {
        fprintf(out, "    .align 8\nfor_iter_%d:\n    .zero %ld\n", i, SLOT_SIZE);
        fprintf(out, "    .align 8\nfor_end_%d:\n    .zero %ld\n", i, SLOT_SIZE);
    }

    free(cg.body_buf);
    free(cg.rodata_buf);
    return 0;
}
