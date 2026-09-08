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
    VOTokenType var_type;
    int is_const;
    int is_hard;         /* HARD-declared: immutable */
    int is_loop_var;
    char vma[16];
    int autocleans;
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

static void scope_pop_last(Scope *s) {
    if (s->count > 0) {
        free(s->symbols[s->count - 1].name);
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
    VOTokenType type;
    char *owner;
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

static VmaSlot *vma_alloc_next(VmaTable *t, VOTokenType type, const char *owner) {
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

static VmaSlot *vma_alloc_specific(VmaTable *t, const char *addr, VOTokenType type, const char *owner) {
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
 * Label table
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
        case NODE_JOB_DECL:
            collect_labels(node->as.job_decl.body, labels, had_error);
            break;
        case NODE_DO_STMT:
            collect_labels(node->as.do_stmt.try_block, labels, had_error);
            collect_labels(node->as.do_stmt.catch_block, labels, had_error);
            break;
        case NODE_PEICE_DECL:
            collect_labels(node->as.peice_decl.body, labels, had_error);
            break;
        default:
            break;
    }
}

/* =======================================================================
 * Job registry
 * ===================================================================== */
typedef struct {
    char *name;
    int param_count;
} JobReg;

static JobReg *job_lookup(JobReg *jobs, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (strcmp(jobs[i].name, name) == 0) return &jobs[i];
    return NULL;
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
    int in_job;          /* inside a JOB body */
    int in_peice;        /* inside a PEICE */
    int had_error;
    JobReg *jobs;
    int job_count, job_cap;
} Analyzer;

static void job_register(Analyzer *a, const char *name, int param_count, int line) {
    if (job_lookup(a->jobs, a->job_count, name)) {
        sem_error(line, "job '%s' is already defined", name);
        a->had_error = 1;
        return;
    }
    if (a->job_count == a->job_cap) {
        a->job_cap = a->job_cap ? a->job_cap * 2 : 8;
        a->jobs = realloc(a->jobs, sizeof(JobReg) * a->job_cap);
    }
    JobReg *j = &a->jobs[a->job_count++];
    j->name = strdup(name);
    j->param_count = param_count;
}

static void collect_jobs(ASTNode *node, Analyzer *a) {
    if (!node) return;

    switch (node->type) {
        case NODE_JOB_DECL:
            job_register(a, node->as.job_decl.name, node->as.job_decl.param_count, node->line);
            collect_jobs(node->as.job_decl.body, a);
            break;
        case NODE_BLOCK:
        case NODE_PROGRAM:
            for (int i = 0; i < node->as.block.statements.count; i++)
                collect_jobs(node->as.block.statements.items[i], a);
            break;
        case NODE_PEICE_DECL:
            /* PEICE is NOT a block node: its body is a separate BLOCK node
               (union field peice_decl.body). Reading as.block.statements on
               the PEICE node reinterprets name/body pointers as a NodeList
               (garbage count from a heap address -> random crashes). */
            collect_jobs(node->as.peice_decl.body, a);
            break;
        case NODE_IF_STMT:
            for (int i = 0; i < node->as.if_stmt.branches.count; i++)
                collect_jobs(node->as.if_stmt.branches.items[i], a);
            break;
        case NODE_IF_BRANCH:
            collect_jobs(node->as.if_branch.block, a);
            break;
        case NODE_WHILE_STMT:
            collect_jobs(node->as.while_stmt.block, a);
            break;
        case NODE_FOR_STMT:
            collect_jobs(node->as.for_stmt.block, a);
            break;
        case NODE_WHEN_STMT:
            collect_jobs(node->as.when_stmt.block, a);
            break;
        case NODE_DO_STMT:
            collect_jobs(node->as.do_stmt.try_block, a);
            collect_jobs(node->as.do_stmt.catch_block, a);
            break;
        default:
            break;
    }
}

static void analyze_stmt(Analyzer *a, ASTNode *node);
static void analyze_expr(Analyzer *a, ASTNode *node);

static void analyze_block(Analyzer *a, ASTNode *block) {
    if (!block) return;
    for (int i = 0; i < block->as.block.statements.count; i++)
        analyze_stmt(a, block->as.block.statements.items[i]);
}

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
            if (sym->is_const || sym->is_hard) {
                sem_error(target->line, "cannot assign to '%s' - declared at line %d",
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
        case NODE_MODULE_REF:
            /* Module-qualified writes are allowed (e.g. bank.Balance = X) */
            break;
        default:
            break;
    }
}

static const char *command_kind_name(CommandKind kind) {
    switch (kind) {
        case CMD_ATTACH:   return "ATTACH";
        case CMD_PLACE:    return "PLACE";
        case CMD_ERASE:    return "ERASE";
        case CMD_COUNT:    return "COUNT";
        case CMD_TAKE:     return "TAKE";
        case CMD_SEEK:     return "SEEK";
        case CMD_HAS:      return "HAS";
        case CMD_BIND:     return "BIND";
        case CMD_SEVER:    return "SEVER";
        case CMD_CUT:      return "CUT";
        case CMD_RAISE:    return "RAISE";
        case CMD_LOWER:    return "LOWER";
        case CMD_UNSEAL:   return "UNSEAL";
        case CMD_SEAL:     return "SEAL";
        case CMD_DRAW:     return "DRAW";
        case CMD_PUT:      return "PUT";
        case CMD_MOVE:     return "MOVE";
        case CMD_MAKE:     return "MAKE";
        case CMD_RECALL:   return "RECALL";
        case CMD_CLONE:    return "CLONE";
        case CMD_DELIVER:  return "DELIVER";
        case CMD_SPAWN:    return "SPAWN";
        case CMD_HOLD:     return "HOLD";
        case CMD_CLAIM:    return "CLAIM";
        case CMD_HALT:     return "HALT";
        case CMD_SEIZE:    return "SEIZE";
        case CMD_RELEASE:  return "RELEASE";
        case CMD_ALIGN:    return "ALIGN";
        case CMD_ARM:      return "ARM";
        case CMD_DISARM:   return "DISARM";
        case CMD_FIRE:     return "FIRE";
        case CMD_RANK:     return "RANK";
        case CMD_KILL:     return "KILL";
        case CMD_SCREEN:   return "SCREEN";
        case CMD_LINK:     return "LINK";
    }
    return "COMMAND";
}

/* Operand arity per spec sec. 27/31-36/40-60, matching exec_command(). */
static int command_min_max(CommandKind kind, int *min_out, int *max_out) {
    switch (kind) {
        case CMD_ATTACH:   *min_out = 2; *max_out = 2; break;
        case CMD_PLACE:    *min_out = 3; *max_out = 3; break;
        case CMD_ERASE:    *min_out = 1; *max_out = 2; break;
        case CMD_COUNT:    *min_out = 1; *max_out = 1; break;
        case CMD_TAKE:     *min_out = 1; *max_out = 1; break;
        case CMD_SEEK:     *min_out = 2; *max_out = 2; break;
        case CMD_HAS:      *min_out = 1; *max_out = 2; break;
        case CMD_BIND:     *min_out = 2; *max_out = 2; break;
        case CMD_SEVER:    *min_out = 2; *max_out = 2; break;
        case CMD_CUT:      *min_out = 1; *max_out = 1; break;
        case CMD_RAISE:    *min_out = 1; *max_out = 1; break;
        case CMD_LOWER:    *min_out = 1; *max_out = 1; break;
        case CMD_UNSEAL:   *min_out = 1; *max_out = 2; break;
        case CMD_SEAL:     *min_out = 1; *max_out = 1; break;
        case CMD_DRAW:     *min_out = 1; *max_out = 2; break;
        case CMD_PUT:      *min_out = 2; *max_out = 2; break;
        case CMD_MOVE:     *min_out = 2; *max_out = 2; break;
        case CMD_MAKE:     *min_out = 1; *max_out = 1; break;
        case CMD_RECALL:   *min_out = 2; *max_out = 2; break;
        case CMD_CLONE:    *min_out = 2; *max_out = 2; break;
        case CMD_DELIVER:  *min_out = 2; *max_out = 2; break;
        case CMD_SPAWN:    *min_out = 1; *max_out = 1; break;
        case CMD_HOLD:     *min_out = 1; *max_out = 1; break;
        case CMD_CLAIM:    *min_out = 1; *max_out = 1; break;
        case CMD_HALT:     *min_out = 1; *max_out = 1; break;
        case CMD_SEIZE:    *min_out = 1; *max_out = 1; break;
        case CMD_RELEASE:  *min_out = 1; *max_out = 1; break;
        case CMD_ALIGN:    *min_out = 1; *max_out = 1; break;
        case CMD_ARM:      *min_out = 1; *max_out = 1; break;
        case CMD_DISARM:   *min_out = 1; *max_out = 1; break;
        case CMD_FIRE:     *min_out = 1; *max_out = 1; break;
        case CMD_KILL:     *min_out = 1; *max_out = 1; break;
        case CMD_SCREEN:   *min_out = 2; *max_out = 2; break;
        case CMD_RANK:     *min_out = 2; *max_out = 2; break;
        case CMD_LINK:     *min_out = 2; *max_out = 2; break;
        default: return 0;
    }
    return 1;
}

/* Best-effort static type of an expression: literals and declared
   identifiers only. Unknown expressions (calls, commands, slices) and EMP
   (variant) return -1 / skip so the runtime decides. */
static VOTokenType expr_static_type(Analyzer *a, ASTNode *node) {
    if (!node) return (VOTokenType)-1;
    switch (node->type) {
        case NODE_IDENTIFIER: {
            Symbol *sym = scope_resolve(a->scope, node->as.identifier.name);
            return sym ? sym->var_type : (VOTokenType)-1;
        }
        case NODE_ARRAY_LITERAL: return TOKEN_TYPE_COLL;
        case NODE_TEX_LITERAL:   return TOKEN_TYPE_TEX;
        case NODE_NUM_LITERAL:   return TOKEN_TYPE_NUM;
        case NODE_DEC_LITERAL:   return TOKEN_TYPE_DEC;
        case NODE_BOOL_LITERAL:  return TOKEN_TYPE_YN;
        case NODE_EMP_LITERAL:   return TOKEN_TYPE_EMP;
        default: return (VOTokenType)-1;
    }
}

static const char *type_display_name(VOTokenType t) {
    switch (t) {
        case TOKEN_TYPE_NUM:   return "NUM";
        case TOKEN_TYPE_DEC:   return "DEC";
        case TOKEN_TYPE_TEX:   return "TEX";
        case TOKEN_TYPE_YN:    return "YN";
        case TOKEN_TYPE_COLL:  return "COLL";
        case TOKEN_TYPE_EMP:   return "EMP";
        case TOKEN_TYPE_FILE:  return "FILE";
        case TOKEN_TYPE_TASK:  return "TASK";
        case TOKEN_TYPE_LOCK:  return "LOCK";
        case TOKEN_TYPE_EVENT: return "EVENT";
        default: return "value";
    }
}

static void check_command_operand_type(Analyzer *a, CommandKind kind,
                                       ASTNode *operand, int position,
                                       VOTokenType expected, VOTokenType alt) {
    VOTokenType t = expr_static_type(a, operand);
    if (t == (VOTokenType)-1 || t == TOKEN_TYPE_EMP) return; /* unknown/variant */
    if (t == expected || (alt != (VOTokenType)-1 && t == alt)) return;
    const char *what = operand->type == NODE_IDENTIFIER
                       ? operand->as.identifier.name : "this expression";
    sem_error(operand->line, "%s operand %d expects a %s value but '%s' is %s",
              command_kind_name(kind), position, type_display_name(expected),
              what, type_display_name(t));
    a->had_error = 1;
}

/* NODE_COMMAND validation: arity (spec sec. 27/31-36/40-60) plus
   static-type conformance where the operand's declared type is known. */
static void analyze_command(Analyzer *a, ASTNode *node) {
    CommandKind kind = node->as.command.kind;
    int min, max;
    if (!command_min_max(kind, &min, &max)) return;
    int n = node->as.command.args.count;
    int line = node->line;

    if (n < min || n > max) {
        if (min == max)
            sem_error(line, "%s expects %d operand(s) but got %d", command_kind_name(kind), min, n);
        else
            sem_error(line, "%s expects %d to %d operands but got %d", command_kind_name(kind), min, max, n);
        a->had_error = 1;
    }

    for (int i = 0; i < n; i++)
        analyze_expr(a, node->as.command.args.items[i]);

    if (n < min || n > max) return; /* arity already reported */

    switch (kind) {
        case CMD_ATTACH:
        case CMD_PLACE:
        case CMD_BIND: {
            ASTNode *op = node->as.command.args.items[0];
            ASTNode *t = op;
            if (t->type == NODE_INDEX) t = t->as.index_expr.array; /* list[0] ref */
            check_command_operand_type(a, kind, t, 1, TOKEN_TYPE_COLL, (VOTokenType)-1);
            break;
        }
        case CMD_ERASE:
            if (n == 2) {
                ASTNode *t = node->as.command.args.items[0];
                if (t->type == NODE_INDEX) t = t->as.index_expr.array;
                check_command_operand_type(a, kind, t, 1, TOKEN_TYPE_COLL, (VOTokenType)-1);
            }
            break;
        case CMD_SEVER:
        case CMD_CUT:
        case CMD_RAISE:
        case CMD_LOWER:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_TEX, (VOTokenType)-1);
            break;
        case CMD_SEAL:
        case CMD_DRAW:
        case CMD_PUT:
        case CMD_MOVE:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_FILE, (VOTokenType)-1);
            break;
        case CMD_CLAIM:
        case CMD_HALT:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_TASK, (VOTokenType)-1);
            break;
        case CMD_SEIZE:
        case CMD_RELEASE:
        case CMD_ALIGN:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_LOCK, (VOTokenType)-1);
            break;
        case CMD_ARM:
        case CMD_DISARM:
        case CMD_FIRE:
        case CMD_KILL:
        case CMD_SCREEN:
        case CMD_RANK:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_EVENT, (VOTokenType)-1);
            break;
        case CMD_LINK:
            check_command_operand_type(a, kind, node->as.command.args.items[0], 1,
                                       TOKEN_TYPE_EVENT, (VOTokenType)-1);
            check_command_operand_type(a, kind, node->as.command.args.items[1], 2,
                                       TOKEN_TYPE_EVENT, (VOTokenType)-1);
            break;
        case CMD_SPAWN: {
            ASTNode *op = node->as.command.args.items[0];
            if (op->type == NODE_CALL) {
                ASTNode *callee = op->as.call.callee;
                if (callee->type == NODE_IDENTIFIER) {
                    if (!job_lookup(a->jobs, a->job_count, callee->as.identifier.name)) {
                        sem_error(op->line, "SPAWN references undeclared job '%s'",
                                  callee->as.identifier.name);
                        a->had_error = 1;
                    }
                } else if (callee->type == NODE_MODULE_REF) {
                    /* module member jobs are resolved at runtime */
                } else {
                    sem_error(op->line, "SPAWN expects a job call");
                    a->had_error = 1;
                }
            } else {
                sem_error(op->line, "SPAWN expects a job call");
                a->had_error = 1;
            }
            break;
        }
        default:
            break;
    }
}

static void analyze_expr(Analyzer *a, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_NUM_LITERAL:
        case NODE_DEC_LITERAL:
        case NODE_TEX_LITERAL:
        case NODE_BOOL_LITERAL:
        case NODE_EMP_LITERAL:
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

        case NODE_SLICE:
            analyze_expr(a, node->as.slice_expr.array);
            analyze_expr(a, node->as.slice_expr.start);
            analyze_expr(a, node->as.slice_expr.end);
            break;

        case NODE_TEX_INTERP:
            for (int i = 0; i < node->as.tex_interp.parts.count; i++)
                analyze_expr(a, node->as.tex_interp.parts.items[i]);
            break;

        case NODE_INTERP_LITERAL:
            break;

        case NODE_COMMAND:
            analyze_command(a, node);
            break;

        case NODE_CALL: {
            ASTNode *callee = node->as.call.callee;
            if (callee->type == NODE_MODULE_REF) {
                /* module member calls are resolved at runtime */
            } else if (callee->type == NODE_IDENTIFIER) {
                const char *name = callee->as.identifier.name;
                JobReg *job = job_lookup(a->jobs, a->job_count, name);
                if (!job) {
                    sem_error(node->line, "undeclared job '%s'", name);
                    a->had_error = 1;
                } else if (job->param_count != node->as.call.args.count) {
                    sem_error(node->line, "job '%s' expects %d argument(s) but got %d",
                              name, job->param_count, node->as.call.args.count);
                    a->had_error = 1;
                }
            } else {
                sem_error(node->line, "invalid call target");
                a->had_error = 1;
            }
            for (int i = 0; i < node->as.call.args.count; i++)
                analyze_expr(a, node->as.call.args.items[i]);
            break;
        }

        case NODE_MODULE_REF:
            /* Module references are resolved at runtime */
            break;

        default:
            break;
    }
}

static void analyze_var_decl(Analyzer *a, ASTNode *node) {
    const char *name = node->as.var_decl.name;
    VOTokenType var_type = node->as.var_decl.var_type;
    ASTNode *init = node->as.var_decl.init;
    int is_aliasing_form = init && init->type == NODE_VMA_REF;

    if (!is_aliasing_form) analyze_expr(a, init);

    if (scope_find_local(a->scope, name)) {
        sem_error(node->line, "'%s' is already declared in this scope", name);
        a->had_error = 1;
        return;
    }

    VmaSlot *slot;
    if (is_aliasing_form) {
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
    sym->line = node->line;
    sym->autocleans = a->autoclean_on;
    snprintf(sym->vma, sizeof(sym->vma), "%s", slot->address);
}

static void analyze_hard_decl(Analyzer *a, ASTNode *node) {
    const char *name = node->as.hard_decl.name;
    VOTokenType var_type = node->as.hard_decl.var_type;

    analyze_expr(a, node->as.hard_decl.value);

    if (scope_find_local(a->scope, name)) {
        sem_error(node->line, "'%s' is already declared in this scope", name);
        a->had_error = 1;
        return;
    }

    VmaSlot *slot = vma_alloc_next(&a->vmas, var_type, name);

    Symbol *sym = scope_declare(a->scope, name);
    sym->var_type = var_type;
    sym->is_hard = 1;
    sym->is_const = 1;
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
                "CLEAN %s targets a VMA with no allocation reaching this point", target);
        }
        vma_free(&a->vmas, target);
        return;
    }

    Symbol *sym = scope_resolve(a->scope, target);
    if (!sym) {
        sem_error(node->line, "undeclared identifier '%s'", target);
        a->had_error = 1;
        return;
    }
    if (sym->vma[0]) vma_free(&a->vmas, sym->vma);
}

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

/* Resolve a STORE/WHEN target string: a declared variable name maps to its
   auto-allocated VMA; a raw VMA address (e.g. "A1") is used as-is. Reports
   and returns NULL if the name is an undeclared identifier. */
static const char *analyzer_target_vma(Analyzer *a, const char *name, int line) {
    Symbol *sym = scope_resolve(a->scope, name);
    if (sym) return sym->is_loop_var ? NULL : sym->vma;
    if (is_vma_format(name)) return name;
    sem_error(line, "undeclared identifier '%s'", name);
    a->had_error = 1;
    return NULL;
}

static void analyze_when_stmt(Analyzer *a, ASTNode *node) {
    if (node->as.when_stmt.kind == WHEN_VMA_CHANGED) {
        const char *addr = analyzer_target_vma(a, node->as.when_stmt.vma_name, node->line);
        if (addr) check_vma_access(a, addr, node->line, "read");
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

static void analyze_job_decl(Analyzer *a, ASTNode *node) {
    a->scope = scope_push(a->scope);

    int was_in_job = a->in_job;
    a->in_job = 1;

    /* Declare params in the job's scope */
    for (int i = 0; i < node->as.job_decl.param_count; i++) {
        const char *pname = node->as.job_decl.params[i].name;
        if (scope_find_local(a->scope, pname)) {
            sem_error(node->line, "duplicate parameter name '%s' in job", pname);
            a->had_error = 1;
            continue;
        }
        Symbol *sym = scope_declare(a->scope, pname);
        sym->var_type = node->as.job_decl.params[i].var_type;
        sym->line = node->line;
    }

    analyze_block(a, node->as.job_decl.body);

    a->in_job = was_in_job;
    scope_pop_with_autoclean(a);
}

static void analyze_do_stmt(Analyzer *a, ASTNode *node) {
    a->scope = scope_push(a->scope);
    analyze_block(a, node->as.do_stmt.try_block);
    scope_pop_with_autoclean(a);

    a->scope = scope_push(a->scope);
    analyze_block(a, node->as.do_stmt.catch_block);
    scope_pop_with_autoclean(a);
}

static void analyze_stmt(Analyzer *a, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_VAR_DECL:
            analyze_var_decl(a, node);
            break;

        case NODE_COMMAND:
            analyze_command(a, node);
            break;

        case NODE_HARD_DECL:
            analyze_hard_decl(a, node);
            break;

        case NODE_EXPR_STMT:
            analyze_expr(a, node->as.expr_stmt.expr);
            break;

        case NODE_INC_DEC_STMT:
            analyze_write_target(a, node->as.inc_dec.target);
            break;

        case NODE_SHOW_STMT:
            for (int i = 0; i < node->as.show_stmt.expr.count; i++)
                analyze_expr(a, node->as.show_stmt.expr.items[i]);
            break;

        case NODE_STORE_STMT: {
            analyze_expr(a, node->as.store_stmt.value);
            const char *addr = analyzer_target_vma(a, node->as.store_stmt.target_vma, node->line);
            if (addr) check_vma_access(a, addr, node->line, "write");
            break;
        }

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
                analyze_expr(a, branch->as.if_branch.condition);
                analyze_block(a, branch->as.if_branch.block);
            }
            break;

        case NODE_WHILE_STMT:
            analyze_expr(a, node->as.while_stmt.condition);
            analyze_block(a, node->as.while_stmt.block);
            break;

        case NODE_FOR_STMT:
            analyze_expr(a, node->as.for_stmt.start);
            analyze_expr(a, node->as.for_stmt.end);
            {
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
            break;

        case NODE_GIVE_STMT:
            if (!a->in_job) {
                sem_error(node->line, "GIVE may only be used inside a JOB");
                a->had_error = 1;
            }
            if (node->as.give_stmt.value)
                analyze_expr(a, node->as.give_stmt.value);
            break;

        case NODE_DEMAND_STMT:
            analyze_expr(a, node->as.demand_stmt.condition);
            break;

        case NODE_SERVE_STMT:
            analyze_expr(a, node->as.serve_stmt.value);
            break;

        case NODE_DO_STMT:
            analyze_do_stmt(a, node);
            break;

        case NODE_JOB_DECL:
            analyze_job_decl(a, node);
            break;

        case NODE_BRING_STMT:
            /* Module loading is validated at runtime */
            break;

        case NODE_SHIP_STMT:
            if (!a->in_peice) {
                sem_error(node->line, "SHIP may only be used inside a PEICE");
                a->had_error = 1;
            }
            break;

        case NODE_PEICE_DECL: {
            int was_in_peice = a->in_peice;
            a->in_peice = 1;
            analyze_block(a, node->as.peice_decl.body);
            a->in_peice = was_in_peice;
            break;
        }

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
    collect_jobs(program, &a);

    a.scope = scope_push(NULL);
    analyze_block(&a, program);
    scope_pop_with_autoclean(&a);

    for (int i = 0; i < a.vmas.count; i++) free(a.vmas.slots[i].owner);
    free(a.vmas.slots);
    for (int i = 0; i < a.labels.count; i++) free(a.labels.names[i]);
    free(a.labels.names);
    for (int i = 0; i < a.job_count; i++) free(a.jobs[i].name);
    free(a.jobs);

    return a.had_error;
}
