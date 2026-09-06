#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "analyzer.h"

/* =======================================================================
 * Diagnostics
 * ===================================================================== */
static void sem_error(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[line %d] Semantic error: ", line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void sem_warning(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[line %d] warning: ", line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* =======================================================================
 * Symbol table (scoped)
 * ===================================================================== */
typedef struct {
    char *name;
    TokenType var_type;   /* TOKEN_TYPE_NUM / _DEC / _TEX / _YN / _COLL */
    int is_const;
    int is_loop_var;       /* implicit FOR iterator - no VMA of its own */
    char vma[16];           /* owned VMA address, e.g. "A1"; empty if loop var */
    int autocleans;         /* AUTOCLEAN was ON when this was declared */
    int line;
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

/* Removes the most-recently-declared symbol (used to scope a FOR
   loop's iterator to just that loop, without FOR opening a real
   scope per spec Sec 6.D). */
static void scope_pop_last(Scope *s) {
    if (s->count > 0) {
        free(s->symbols[s->count - 1].name);
        s->count--;
    }
}

/* =======================================================================
 * VMA table
 *
 * A VMA address ("A1", "AA37", ...) is mapped to a single canonical
 * integer so the whole address space (spec Sec 6.A: numeric suffix
 * exhausts 1..9999 before the letter part advances) has one total
 * order, and "first-fit, freed-before-never-used" (Sec 6.B) reduces to
 * "smallest currently-unallocated index".
 *
 * Design note (flagged, not fully spec'd): the address grammar
 * (`[A-Z]+[0-9]+`) doesn't bound the numeric suffix, but every example
 * in the spec caps it at 9999 before the letter part advances,  so
 * that's what VMA_NUMBERS_PER_LETTER encodes. If the real numeric
 * range differs, update that constant only - the rest of the model
 * doesn't depend on the specific value.
 * ===================================================================== */
#define VMA_NUMBERS_PER_LETTER 9999

typedef struct {
    long index;
    char address[16];
    int allocated;
    TokenType type;
    char *owner;      /* identifier bound here, or NULL if raw-addressed */
} VmaSlot;

typedef struct {
    VmaSlot *slots;
    int count, capacity;
} VmaTable;

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

/* Finds the slot tracking `idx`, creating an (initially unallocated)
   one if this address has never been touched before. Slots are kept
   around after being freed so type/owner history and address-string
   formatting don't need to be recomputed. */
static VmaSlot *vma_slot_for_index(VmaTable *t, long idx) {
    for (int i = 0; i < t->count; i++)
        if (t->slots[i].index == idx) return &t->slots[i];

    if (t->count == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 8;
        t->slots = realloc(t->slots, sizeof(VmaSlot) * t->capacity);
    }
    VmaSlot *slot = &t->slots[t->count++];
    memset(slot, 0, sizeof(VmaSlot));
    slot->index = idx;
    index_to_vma(idx, slot->address);
    return slot;
}

static VmaSlot *vma_lookup(VmaTable *t, const char *addr) {
    long idx = vma_canonical_index(addr);
    for (int i = 0; i < t->count; i++)
        if (t->slots[i].index == idx) return &t->slots[i];
    return NULL;
}

/* First-fit: the smallest index not currently allocated. Freed slots
   are automatically preferred over addresses that have never been
   touched, because they always sort lower than the allocation
   frontier - no separate free-list bookkeeping needed. */
static VmaSlot *vma_alloc_next(VmaTable *t, TokenType type, const char *owner) {
    long idx = 1;
    for (;;) {
        VmaSlot *existing = NULL;
        for (int i = 0; i < t->count; i++)
            if (t->slots[i].index == idx) { existing = &t->slots[i]; break; }
        if (!existing || !existing->allocated) break;
        idx++;
    }
    VmaSlot *slot = vma_slot_for_index(t, idx);
    slot->allocated = 1;
    slot->type = type;
    free(slot->owner);
    slot->owner = owner ? strdup(owner) : NULL;
    return slot;
}

/* Direct-address allocation, for the `VAR ... EAQ <VMA>` aliasing
   form. Returns NULL if that address is already allocated (aliasing
   ban, spec Sec 5) so the caller can report it. */
static VmaSlot *vma_alloc_specific(VmaTable *t, const char *addr, TokenType type, const char *owner) {
    long idx = vma_canonical_index(addr);
    VmaSlot *slot = vma_slot_for_index(t, idx);
    if (slot->allocated) return NULL;
    slot->allocated = 1;
    slot->type = type;
    free(slot->owner);
    slot->owner = owner ? strdup(owner) : NULL;
    return slot;
}

static void vma_free(VmaTable *t, const char *addr) {
    VmaSlot *slot = vma_lookup(t, addr);
    if (!slot) return;
    slot->allocated = 0;
    free(slot->owner);
    slot->owner = NULL;
}

static void vma_free_all(VmaTable *t) {
    for (int i = 0; i < t->count; i++) {
        t->slots[i].allocated = 0;
        free(t->slots[i].owner);
        t->slots[i].owner = NULL;
    }
}

/* =======================================================================
 * Label table (flat, whole-program - GOTO in this assembly-flavored
 * language isn't scoped, so labels are collected up front in a
 * pre-pass and don't need forward-declaration).
 * ===================================================================== */
typedef struct {
    char **names;
    int count, capacity;
} LabelTable;

static int label_exists(LabelTable *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->names[i], name) == 0) return 1;
    return 0;
}

static void label_add(LabelTable *t, const char *name) {
    if (t->count == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 8;
        t->names = realloc(t->names, sizeof(char *) * t->capacity);
    }
    t->names[t->count++] = strdup(name);
}

static void collect_labels(ASTNode *node, LabelTable *labels, int *had_error) {
    if (!node) return;

    if (node->type == NODE_LABEL_STMT) {
        if (label_exists(labels, node->as.label_stmt.label)) {
            sem_error(node->line, "label '%s' is already defined elsewhere in this program",
                      node->as.label_stmt.label);
            *had_error = 1;
        } else {
            label_add(labels, node->as.label_stmt.label);
        }
        return;
    }

    /* Walk every construct that can contain statements. */
    switch (node->type) {
        case NODE_BLOCK:
        case NODE_PROGRAM:
            for (int i = 0; i < node->as.block.statements.count; i++)
                collect_labels(node->as.block.statements.items[i], labels, had_error);
            break;
        case NODE_IF_STMT:
            for (int i = 0; i < node->as.if_stmt.branches.count; i++)
                collect_labels(node->as.if_stmt.branches.items[i], labels, had_error);
            break;
        case NODE_IF_BRANCH:
            collect_labels(node->as.if_branch.block, labels, had_error);
            break;
        case NODE_WHILE_STMT:
            collect_labels(node->as.while_stmt.block, labels, had_error);
            break;
        case NODE_FOR_STMT:
            collect_labels(node->as.for_stmt.block, labels, had_error);
            break;
        case NODE_WHEN_STMT:
            collect_labels(node->as.when_stmt.block, labels, had_error);
            break;
        default:
            break; /* not a container of statements */
    }
}

/* =======================================================================
 * Analyzer state + core passes
 * ===================================================================== */
typedef struct {
    VmaTable vmas;
    LabelTable labels;
    Scope *scope;
    int autoclean_on;
    int in_changed_handler;
    int had_error;
} Analyzer;

static void analyze_stmt(Analyzer *a, ASTNode *node);
static void analyze_expr(Analyzer *a, ASTNode *node);

static void analyze_block(Analyzer *a, ASTNode *block) {
    if (!block) return;
    for (int i = 0; i < block->as.block.statements.count; i++)
        analyze_stmt(a, block->as.block.statements.items[i]);
}

/* Soft, flow-insensitive check for a direct read/write of a raw VMA
   address (see the design note in analyzer.h/above vma_alloc_next).
   Never sets had_error - a single linear pass over the AST can't
   soundly know whether a conditionally-allocated address is live at
   this point, so this is advisory only. */
static void check_vma_access(Analyzer *a, const char *addr, int line, const char *verb) {
    VmaSlot *slot = vma_lookup(&a->vmas, addr);
    if (!slot || !slot->allocated) {
        sem_warning(line,
            "%s of VMA %s with no allocation reaching this point in program order "
            "(this check is best-effort and control-flow-insensitive - ignore it "
            "if a preceding branch/loop allocates %s on the path that matters)",
            verb, addr, addr);
    }
}

static int is_reserved_event_binding(const char *name) {
    return strcmp(name, "OLD_VALUE") == 0 || strcmp(name, "NEW_VALUE") == 0;
}

/* Resolves either an identifier or a raw VMA as an assignment/++/--
   target, applying the const-write and undeclared-identifier checks.
   `value_analyzed_ok` is only used for warnings.  */
static void analyze_write_target(Analyzer *a, ASTNode *target) {
    switch (target->type) {
        case NODE_IDENTIFIER: {
            const char *name = target->as.identifier.name;
            Symbol *sym = scope_resolve(a->scope, name);
            if (!sym) {
                sem_error(target->line, "undeclared identifier '%s'", name);
                a->had_error = 1;
                return;
            }
            if (sym->is_const) {
                sem_error(target->line, "cannot assign to '%s' - declared CONST at line %d",
                          name, sym->line);
                a->had_error = 1;
            }
            break;
        }
        case NODE_VMA_REF:
            check_vma_access(a, target->as.vma_ref.name, target->line, "write");
            break;
        case NODE_INDEX:
            analyze_expr(a, target->as.index_expr.array);
            analyze_expr(a, target->as.index_expr.index);
            break;
        default:
            break; /* parser already rejects other shapes */
    }
}

static void analyze_expr(Analyzer *a, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_NUM_LITERAL:
        case NODE_DEC_LITERAL:
        case NODE_TEX_LITERAL:
        case NODE_BOOL_LITERAL:
        case NODE_NULL_LITERAL:
            break;

        case NODE_VMA_REF:
            check_vma_access(a, node->as.vma_ref.name, node->line, "read");
            break;

        case NODE_IDENTIFIER: {
            const char *name = node->as.identifier.name;
            if (is_reserved_event_binding(name)) {
                if (!a->in_changed_handler) {
                    sem_error(node->line,
                        "%s may only be used inside a WHEN <vma> CHANGED handler", name);
                    a->had_error = 1;
                }
                break;
            }
            if (!scope_resolve(a->scope, name)) {
                sem_error(node->line, "undeclared identifier '%s'", name);
                a->had_error = 1;
            }
            break;
        }

        case NODE_UNARY:
            analyze_expr(a, node->as.unary.operand);
            break;

        case NODE_BINARY:
            analyze_expr(a, node->as.binary.left);
            analyze_expr(a, node->as.binary.right);
            break;

        case NODE_ASSIGN:
            /* value first: `X = X + 1` reads the old X before the
               write is checked, and evaluating value can't be
               affected by the target being invalid. */
            analyze_expr(a, node->as.assign.value);
            analyze_write_target(a, node->as.assign.target);
            break;

        case NODE_LOAD:
            analyze_expr(a, node->as.load.vma);
            break;

        case NODE_LENGTH_CALL:
            analyze_expr(a, node->as.length_call.arg);
            break;

        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                analyze_expr(a, node->as.array_lit.elements.items[i]);
            break;

        case NODE_INDEX:
            analyze_expr(a, node->as.index_expr.array);
            analyze_expr(a, node->as.index_expr.index);
            break;

        default:
            /* Statement-shaped nodes never reach here from valid ASTs. */
            break;
    }
}

/* Handles both VAR and CONST decls, including the `EAQ <VMA>` aliasing
   form from spec Sec 5. */
static void analyze_var_decl(Analyzer *a, ASTNode *node) {
    int is_const = node->type == NODE_CONST_DECL;
    const char *name = node->as.var_decl.name;
    TokenType var_type = node->as.var_decl.var_type;
    ASTNode *init = node->as.var_decl.init;
    int is_aliasing_form = init && init->type == NODE_VMA_REF;

    /* Skip the generic analyze_expr() for the `EAQ <VMA>` aliasing
       form - naming an address to establish isn't a read of its
       current value, so it shouldn't trip check_vma_access(). */
    if (!is_aliasing_form) analyze_expr(a, init);

    if (scope_find_local(a->scope, name)) {
        sem_error(node->line, "'%s' is already declared in this scope", name);
        a->had_error = 1;
        return;
    }

    VmaSlot *slot;
    if (is_aliasing_form) {
        /* `VAR NUM X EAQ A1` - bind directly to A1 instead of
           first-fit allocating a fresh address. */
        const char *addr = init->as.vma_ref.name;
        slot = vma_alloc_specific(&a->vmas, addr, var_type, name);
        if (!slot) {
            VmaSlot *existing = vma_lookup(&a->vmas, addr);
            if (existing && existing->owner)
                sem_error(node->line,
                    "cannot bind '%s' to %s - already allocated to '%s' (aliasing is banned)",
                    name, addr, existing->owner);
            else
                sem_error(node->line,
                    "cannot bind '%s' to %s - already allocated (aliasing is banned)",
                    name, addr);
            a->had_error = 1;
            return;
        }
    } else {
        slot = vma_alloc_next(&a->vmas, var_type, name);
    }

    Symbol *sym = scope_declare(a->scope, name);
    sym->var_type = var_type;
    sym->is_const = is_const;
    sym->line = node->line;
    sym->autocleans = a->autoclean_on;
    snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
}

static void analyze_clean_stmt(Analyzer *a, ASTNode *node) {
    const char *target = node->as.clean_stmt.target;

    if (is_vma_format(target)) {
        VmaSlot *slot = vma_lookup(&a->vmas, target);
        if (!slot || !slot->allocated) {
            sem_warning(node->line,
                "CLEAN %s targets a VMA with no allocation reaching this point "
                "(control-flow-insensitive check - fine if an earlier branch handles it)",
                target);
        }
        vma_free(&a->vmas, target);
        return;
    }

    /* Identifier form: `CLEAN Age`. Must already be declared. Freeing
       only affects the VMA table (for allocator reuse); the Symbol
       itself is left in scope, because whether the identifier is
       "live" again afterwards depends on control flow this pass
       doesn't model - see the design note in analyzer.h. */
    Symbol *sym = scope_resolve(a->scope, target);
    if (!sym) {
        sem_error(node->line, "undeclared identifier '%s'", target);
        a->had_error = 1;
        return;
    }
    if (sym->vma[0]) vma_free(&a->vmas, sym->vma);
}

/* Applies AUTOCLEAN: frees the VMA of every symbol declared in `s`
   while AUTOCLEAN was ON, then discards the scope itself. */
static void scope_pop_with_autoclean(Analyzer *a) {
    Scope *s = a->scope;
    for (int i = 0; i < s->count; i++) {
        Symbol *sym = &s->symbols[i];
        if (sym->autocleans && sym->vma[0]) vma_free(&a->vmas, sym->vma);
    }
    a->scope = s->parent;
    for (int i = 0; i < s->count; i++) free(s->symbols[i].name);
    free(s->symbols);
    free(s);
}

static void analyze_when_stmt(Analyzer *a, ASTNode *node) {
    if (node->as.when_stmt.kind == WHEN_VMA_CHANGED) {
        check_vma_access(a, node->as.when_stmt.vma_name, node->line, "read");
    } else if (node->as.when_stmt.kind == WHEN_CONDITION) {
        analyze_expr(a, node->as.when_stmt.condition);
    }

    a->scope = scope_push(a->scope);

    int was_changed_handler = a->in_changed_handler;
    if (node->as.when_stmt.kind == WHEN_VMA_CHANGED) a->in_changed_handler = 1;

    analyze_block(a, node->as.when_stmt.block);

    a->in_changed_handler = was_changed_handler;
    scope_pop_with_autoclean(a);
}

static void analyze_stmt(Analyzer *a, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_CONST_DECL:
            analyze_var_decl(a, node);
            break;

        case NODE_EXPR_STMT:
            analyze_expr(a, node->as.expr_stmt.expr);
            break;

        case NODE_INC_DEC_STMT:
            analyze_write_target(a, node->as.inc_dec.target);
            break;

        case NODE_SHOW_STMT:
            analyze_expr(a, node->as.show_stmt.expr);
            break;

        case NODE_STORE_STMT:
            analyze_expr(a, node->as.store_stmt.value);
            check_vma_access(a, node->as.store_stmt.target_vma, node->line, "write");
            break;

        case NODE_CLEAN_STMT:
            analyze_clean_stmt(a, node);
            break;

        case NODE_CLEANALL_STMT:
            vma_free_all(&a->vmas);
            break;

        case NODE_AUTOCLEAN_STMT:
            a->autoclean_on = node->as.autoclean_stmt.on;
            break;

        case NODE_IF_STMT:
            for (int i = 0; i < node->as.if_stmt.branches.count; i++) {
                ASTNode *branch = node->as.if_stmt.branches.items[i];
                analyze_expr(a, branch->as.if_branch.condition); /* NULL is fine */
                analyze_block(a, branch->as.if_branch.block);    /* no new scope */
            }
            break;

        case NODE_WHILE_STMT:
            analyze_expr(a, node->as.while_stmt.condition);
            analyze_block(a, node->as.while_stmt.block); /* no new scope */
            break;

        case NODE_FOR_STMT:
            analyze_expr(a, node->as.for_stmt.start);
            analyze_expr(a, node->as.for_stmt.end);
            {
                /* Iterator is lexically visible only inside this
                   loop's block; FOR doesn't open a real (AUTOCLEAN)
                   scope, so it gets no VMA of its own - just pushed
                   and popped off the current scope like a stack. */
                Symbol *iter = scope_declare(a->scope, node->as.for_stmt.iterator);
                iter->is_loop_var = 1;
                iter->line = node->line;
                iter->var_type = TOKEN_TYPE_NUM;
                analyze_block(a, node->as.for_stmt.block);
                scope_pop_last(a->scope);
            }
            break;

        case NODE_WHEN_STMT:
            analyze_when_stmt(a, node);
            break;

        case NODE_GOTO_STMT:
            if (!label_exists(&a->labels, node->as.goto_stmt.label)) {
                sem_error(node->line, "GOTO target '%s' is not defined anywhere in this program",
                          node->as.goto_stmt.label);
                a->had_error = 1;
            }
            break;

        case NODE_LABEL_STMT:
            break; /* already validated/collected in the label pre-pass */

        case NODE_BLOCK:
            analyze_block(a, node);
            break;

        default:
            break;
    }
}

/* =======================================================================
 * Public entry point
 * ===================================================================== */
int analyze_program(ASTNode *program) {
    Analyzer a;
    memset(&a, 0, sizeof(a));

    collect_labels(program, &a.labels, &a.had_error);

    a.scope = scope_push(NULL); /* the program's own top-level scope */
    analyze_block(&a, program);
    scope_pop_with_autoclean(&a);

    for (int i = 0; i < a.vmas.count; i++) free(a.vmas.slots[i].owner);
    free(a.vmas.slots);
    for (int i = 0; i < a.labels.count; i++) free(a.labels.names[i]);
    free(a.labels.names);

    return a.had_error;
}
