#ifndef VO_AST_H
#define VO_AST_H

#include "token.h"

typedef struct ASTNode ASTNode;

/* Generic growable list of AST node pointers - used for blocks,
   if/orif branch chains, array literals, argument lists, etc. */
typedef struct {
    ASTNode **items;
    int count;
    int capacity;
} NodeList;

void nodelist_init(NodeList *list);
void nodelist_push(NodeList *list, ASTNode *node);

typedef enum {
    /* ---- Expressions ---- */
    NODE_NUM_LITERAL,
    NODE_DEC_LITERAL,
    NODE_TEX_LITERAL,
    NODE_BOOL_LITERAL,     /* YES / NO   */
    NODE_NULL_LITERAL,
    NODE_VMA_REF,          /* A1, AA3, ... referenced directly */
    NODE_IDENTIFIER,       /* Balance, Status, ... */
    NODE_UNARY,            /* -x, NOT x */
    NODE_BINARY,           /* x + y, x AND y, x ** y, ... */
    NODE_ASSIGN,           /* target = / += / -= / ... value */
    NODE_LOAD,             /* LOAD A1 */
    NODE_LENGTH_CALL,      /* LENGTH(expr) */
    NODE_ARRAY_LITERAL,    /* [1, 2, 3] */
    NODE_INDEX,            /* History[I] */

    /* ---- Statements ---- */
    NODE_VAR_DECL,         /* VAR NUM Age EAQ 45 */
    NODE_CONST_DECL,       /* CONST NUM Age EAQ 45 */
    NODE_EXPR_STMT,        /* wraps a NODE_ASSIGN used as a statement */
    NODE_INC_DEC_STMT,     /* Counter++ / A1-- */
    NODE_SHOW_STMT,        /* SHOW expr */
    NODE_STORE_STMT,       /* STORE value A1 */
    NODE_CLEAN_STMT,       /* CLEAN A1 / CLEAN Age */
    NODE_CLEANALL_STMT,    /* CLEANALL */
    NODE_AUTOCLEAN_STMT,   /* AUTOCLEAN ON/OFF */
    NODE_IF_BRANCH,        /* one condition+block pair inside an if-chain */
    NODE_IF_STMT,          /* the full IF/ORIF/IFNOT/ENDIF chain */
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_WHEN_STMT,
    NODE_GOTO_STMT,
    NODE_LABEL_STMT,
    NODE_BLOCK,
    NODE_PROGRAM
} NodeType;

typedef enum {
    WHEN_VMA_CHANGED,   /* WHEN A1 CHANGED */
    WHEN_CONDITION,     /* WHEN A1 < 100   */
    WHEN_PROGRAM_START  /* WHEN PROGRAM START */
} WhenKind;

struct ASTNode {
    NodeType type;
    int line;

    union {
        /* literals */
        struct { long value; }               num_lit;
        struct { double value; }             dec_lit;
        struct { char *value; }              tex_lit;   /* owned, unescaped */
        struct { int value; }                bool_lit;  /* 1 = YES, 0 = NO */

        struct { char *name; }               vma_ref;
        struct { char *name; }               identifier;

        struct { TokenType op; ASTNode *operand; }                 unary;
        struct { TokenType op; ASTNode *left; ASTNode *right; }    binary;
        struct { TokenType op; ASTNode *target; ASTNode *value; }  assign;

        struct { ASTNode *vma; }             load;          /* LOAD A1 */
        struct { ASTNode *arg; }             length_call;   /* LENGTH(expr) */
        struct { NodeList elements; }        array_lit;
        struct { ASTNode *array; ASTNode *index; } index_expr;

        /* declarations */
        struct { TokenType var_type; char *name; ASTNode *init; } var_decl;

        /* statements */
        struct { ASTNode *expr; }            expr_stmt;
        struct { ASTNode *target; TokenType op; } inc_dec;   /* op = INCREMENT/DECREMENT */
        struct { ASTNode *expr; }            show_stmt;
        struct { ASTNode *value; char *target_vma; } store_stmt;
        struct { char *target; }             clean_stmt;     /* identifier or VMA text */
        struct { int on; }                   autoclean_stmt;

        struct { ASTNode *condition; ASTNode *block; } if_branch; /* condition==NULL => else */
        struct { NodeList branches; }        if_stmt;             /* list of NODE_IF_BRANCH */

        struct { ASTNode *condition; ASTNode *block; } while_stmt;

        struct { char *iterator; ASTNode *start; ASTNode *end; ASTNode *block; } for_stmt;

        struct {
            WhenKind kind;
            char *vma_name;        /* used when kind == WHEN_VMA_CHANGED */
            ASTNode *condition;    /* used when kind == WHEN_CONDITION   */
            ASTNode *block;
        } when_stmt;

        struct { char *label; }              goto_stmt;
        struct { char *label; }              label_stmt;

        struct { NodeList statements; }      block;
    } as;
};

ASTNode *ast_new(NodeType type, int line);
void ast_print(ASTNode *node, int indent);

#endif
