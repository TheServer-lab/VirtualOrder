#ifndef VO_AST_H
#define VO_AST_H

#include "token.h"

typedef struct ASTNode ASTNode;

/* Generic growable list of AST node pointers */
typedef struct {
    ASTNode **items;
    int count;
    int capacity;
} NodeList;

void nodelist_init(NodeList *list);
void nodelist_push(NodeList *list, ASTNode *node);

/* Job parameter: type + name pair */
typedef struct {
    VOTokenType var_type;
    char *name;
} JobParam;

typedef enum {
    /* ---- Expressions ---- */
    NODE_NUM_LITERAL,
    NODE_DEC_LITERAL,
    NODE_TEX_LITERAL,
    NODE_BOOL_LITERAL,
    NODE_EMP_LITERAL,
    NODE_VMA_REF,
    NODE_IDENTIFIER,
    NODE_UNARY,
    NODE_BINARY,
    NODE_ASSIGN,
    NODE_LOAD,
    NODE_LENGTH_CALL,
    NODE_ARRAY_LITERAL,
    NODE_INDEX,
    NODE_SLICE,
    NODE_TEX_INTERP,
    NODE_INTERP_LITERAL,
    NODE_COMMAND,
    NODE_CALL,
    NODE_MODULE_REF,

    /* ---- Statements ---- */
    NODE_VAR_DECL,
    NODE_HARD_DECL,
    NODE_EXPR_STMT,
    NODE_INC_DEC_STMT,
    NODE_SHOW_STMT,
    NODE_STORE_STMT,
    NODE_CLEAN_STMT,
    NODE_CLEANALL_STMT,
    NODE_AUTOCLEAN_STMT,
    NODE_IF_BRANCH,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_WHEN_STMT,
    NODE_GOTO_STMT,
    NODE_LABEL_STMT,
    NODE_GIVE_STMT,
    NODE_DEMAND_STMT,
    NODE_SERVE_STMT,
    NODE_DO_STMT,
    NODE_BRING_STMT,
    NODE_SHIP_STMT,
    NODE_BLOCK,
    NODE_PROGRAM,

    /* ---- Declarations ---- */
    NODE_JOB_DECL,
    NODE_PEICE_DECL
} NodeType;

typedef enum {
    WHEN_VMA_CHANGED,
    WHEN_CONDITION,
    WHEN_PROGRAM_START
} WhenKind;

/* Builtin command operations (v1.4) - collections, strings, files, concurrency, events */
typedef enum {
    CMD_ATTACH, CMD_PLACE, CMD_ERASE, CMD_COUNT, CMD_TAKE, CMD_SEEK, CMD_HAS,
    CMD_BIND, CMD_SEVER, CMD_CUT, CMD_RAISE, CMD_LOWER,
    CMD_UNSEAL, CMD_SEAL, CMD_DRAW, CMD_PUT, CMD_MOVE,
    CMD_MAKE, CMD_RECALL, CMD_CLONE, CMD_DELIVER,
    CMD_SPAWN, CMD_HOLD, CMD_CLAIM, CMD_HALT,
    CMD_SEIZE, CMD_RELEASE, CMD_ALIGN,
    CMD_ARM, CMD_DISARM, CMD_FIRE, CMD_RANK, CMD_KILL, CMD_SCREEN, CMD_LINK
} CommandKind;

struct ASTNode {
    NodeType type;
    int line;

    union {
        /* literals */
        struct { long value; }               num_lit;
        struct { double value; }             dec_lit;
        struct { char *value; }              tex_lit;
        struct { int value; }                bool_lit;

        struct { char *name; }               vma_ref;
        struct { char *name; }               identifier;

        struct { VOTokenType op; ASTNode *operand; }                 unary;
        struct { VOTokenType op; ASTNode *left; ASTNode *right; }    binary;
        struct { VOTokenType op; ASTNode *target; ASTNode *value; }  assign;

        struct { ASTNode *vma; }             load;
        struct { ASTNode *arg; }             length_call;
        struct { NodeList elements; }        array_lit;
        struct { ASTNode *array; ASTNode *index; } index_expr;
        struct { ASTNode *array; ASTNode *start; ASTNode *end; } slice_expr;

        /* TEX interpolation: literal text node OR expression node per part */
        struct { NodeList parts; }           tex_interp;
        struct { char *text; }               interp_lit;

        /* generic builtin command: kind + operands */
        struct { CommandKind kind; NodeList args; } command;

        /* function call: callee(args...) */
        struct {
            ASTNode *callee;    /* NODE_IDENTIFIER or NODE_MODULE_REF */
            NodeList args;
        } call;

        /* module reference: module.member */
        struct {
            char *module;
            char *member;
        } module_ref;

        /* declarations */
        struct { VOTokenType var_type; char *name; ASTNode *init; } var_decl;
        struct { VOTokenType var_type; char *name; ASTNode *value; } hard_decl;

        /* JOB declaration */
        struct {
            char *name;
            JobParam *params;
            int param_count;
            ASTNode *body;
        } job_decl;

        /* PEICE declaration */
        struct {
            char *name;
            ASTNode *body;
        } peice_decl;

        /* statements */
        struct { ASTNode *expr; }            expr_stmt;
        struct { ASTNode *target; VOTokenType op; } inc_dec;
        struct { NodeList expr; }              show_stmt;
        struct { ASTNode *value; char *target_vma; } store_stmt;
        struct { char *target; }             clean_stmt;
        struct { int on; }                   autoclean_stmt;

        struct { ASTNode *condition; ASTNode *block; } if_branch;
        struct { NodeList branches; }        if_stmt;

        struct { ASTNode *condition; ASTNode *block; } while_stmt;

        struct { char *iterator; ASTNode *start; ASTNode *end; ASTNode *block; } for_stmt;

        struct {
            WhenKind kind;
            char *vma_name;
            ASTNode *condition;
            ASTNode *block;
        } when_stmt;

        struct { char *label; }              goto_stmt;
        struct { char *label; }              label_stmt;

        /* GIVE: return value from JOB */
        struct { ASTNode *value; }           give_stmt;

        /* DEMAND: assertion */
        struct {
            ASTNode *condition;
            char *message;   /* optional, NULL if not provided */
        } demand_stmt;

        /* SERVE: raise an issue */
        struct { ASTNode *value; }           serve_stmt;

        /* DO / GRABE / ENDDO: protected execution */
        struct {
            ASTNode *try_block;
            ASTNode *catch_block;
        } do_stmt;

        /* BRING: import a module */
        struct { char *path; }               bring_stmt;

        /* SHIP: export a member */
        struct { char *name; }               ship_stmt;

        struct { NodeList statements; }      block;
    } as;
};

ASTNode *ast_new(NodeType type, int line);
void ast_print(ASTNode *node, int indent);
void ast_free(ASTNode *node);

#endif
