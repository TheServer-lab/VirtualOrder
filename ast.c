#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

void nodelist_init(NodeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nodelist_push(NodeList *list, ASTNode *node) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(ASTNode *) * list->capacity);
    }
    list->items[list->count++] = node;
}

ASTNode *ast_new(NodeType type, int line) {
    ASTNode *n = calloc(1, sizeof(ASTNode));
    n->type = type;
    n->line = line;
    return n;
}

/* ---------------------------------------------------------------------
 * Debug printer - prints an indented tree so the parser's output can be
 * eyeballed against the source. Not meant to be a pretty-printer that
 * regenerates valid Virtual Order source.
 * ------------------------------------------------------------------- */
static void pad(int indent) { for (int i = 0; i < indent; i++) printf("  "); }

static void print_block(ASTNode *block, int indent) {
    for (int i = 0; i < block->as.block.statements.count; i++) {
        ast_print(block->as.block.statements.items[i], indent);
    }
}

void ast_print(ASTNode *node, int indent) {
    if (!node) { pad(indent); printf("(null)\n"); return; }

    switch (node->type) {
        case NODE_NUM_LITERAL:
            pad(indent); printf("NumLiteral %ld\n", node->as.num_lit.value);
            break;
        case NODE_DEC_LITERAL:
            pad(indent); printf("DecLiteral %g\n", node->as.dec_lit.value);
            break;
        case NODE_TEX_LITERAL:
            pad(indent); printf("TexLiteral \"%s\"\n", node->as.tex_lit.value);
            break;
        case NODE_BOOL_LITERAL:
            pad(indent); printf("BoolLiteral %s\n", node->as.bool_lit.value ? "YES" : "NO");
            break;
        case NODE_NULL_LITERAL:
            pad(indent); printf("NullLiteral\n");
            break;
        case NODE_VMA_REF:
            pad(indent); printf("VmaRef %s\n", node->as.vma_ref.name);
            break;
        case NODE_IDENTIFIER:
            pad(indent); printf("Identifier %s\n", node->as.identifier.name);
            break;
        case NODE_UNARY:
            pad(indent); printf("Unary %s\n", token_type_name(node->as.unary.op));
            ast_print(node->as.unary.operand, indent + 1);
            break;
        case NODE_BINARY:
            pad(indent); printf("Binary %s\n", token_type_name(node->as.binary.op));
            ast_print(node->as.binary.left, indent + 1);
            ast_print(node->as.binary.right, indent + 1);
            break;
        case NODE_ASSIGN:
            pad(indent); printf("Assign %s\n", token_type_name(node->as.assign.op));
            ast_print(node->as.assign.target, indent + 1);
            ast_print(node->as.assign.value, indent + 1);
            break;
        case NODE_LOAD:
            pad(indent); printf("Load\n");
            ast_print(node->as.load.vma, indent + 1);
            break;
        case NODE_LENGTH_CALL:
            pad(indent); printf("LengthCall\n");
            ast_print(node->as.length_call.arg, indent + 1);
            break;
        case NODE_ARRAY_LITERAL:
            pad(indent); printf("ArrayLiteral\n");
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                ast_print(node->as.array_lit.elements.items[i], indent + 1);
            break;
        case NODE_INDEX:
            pad(indent); printf("Index\n");
            ast_print(node->as.index_expr.array, indent + 1);
            ast_print(node->as.index_expr.index, indent + 1);
            break;

        case NODE_VAR_DECL:
        case NODE_CONST_DECL:
            pad(indent);
            printf("%s %s %s\n",
                   node->type == NODE_VAR_DECL ? "VarDecl" : "ConstDecl",
                   token_type_name(node->as.var_decl.var_type),
                   node->as.var_decl.name);
            ast_print(node->as.var_decl.init, indent + 1);
            break;

        case NODE_EXPR_STMT:
            pad(indent); printf("ExprStmt\n");
            ast_print(node->as.expr_stmt.expr, indent + 1);
            break;

        case NODE_INC_DEC_STMT:
            pad(indent); printf("IncDecStmt %s\n", token_type_name(node->as.inc_dec.op));
            ast_print(node->as.inc_dec.target, indent + 1);
            break;

        case NODE_SHOW_STMT:
            pad(indent); printf("ShowStmt\n");
            ast_print(node->as.show_stmt.expr, indent + 1);
            break;

        case NODE_STORE_STMT:
            pad(indent); printf("StoreStmt -> %s\n", node->as.store_stmt.target_vma);
            ast_print(node->as.store_stmt.value, indent + 1);
            break;

        case NODE_CLEAN_STMT:
            pad(indent); printf("CleanStmt %s\n", node->as.clean_stmt.target);
            break;

        case NODE_CLEANALL_STMT:
            pad(indent); printf("CleanAllStmt\n");
            break;

        case NODE_AUTOCLEAN_STMT:
            pad(indent); printf("AutocleanStmt %s\n", node->as.autoclean_stmt.on ? "ON" : "OFF");
            break;

        case NODE_IF_BRANCH:
            pad(indent);
            if (node->as.if_branch.condition) printf("Branch\n");
            else printf("ElseBranch\n");
            if (node->as.if_branch.condition) {
                pad(indent + 1); printf("Condition:\n");
                ast_print(node->as.if_branch.condition, indent + 2);
            }
            pad(indent + 1); printf("Block:\n");
            print_block(node->as.if_branch.block, indent + 2);
            break;

        case NODE_IF_STMT:
            pad(indent); printf("IfStmt\n");
            for (int i = 0; i < node->as.if_stmt.branches.count; i++)
                ast_print(node->as.if_stmt.branches.items[i], indent + 1);
            break;

        case NODE_WHILE_STMT:
            pad(indent); printf("WhileStmt\n");
            pad(indent + 1); printf("Condition:\n");
            ast_print(node->as.while_stmt.condition, indent + 2);
            pad(indent + 1); printf("Block:\n");
            print_block(node->as.while_stmt.block, indent + 2);
            break;

        case NODE_FOR_STMT:
            pad(indent); printf("ForStmt %s\n", node->as.for_stmt.iterator);
            pad(indent + 1); printf("Start:\n");
            ast_print(node->as.for_stmt.start, indent + 2);
            pad(indent + 1); printf("End:\n");
            ast_print(node->as.for_stmt.end, indent + 2);
            pad(indent + 1); printf("Block:\n");
            print_block(node->as.for_stmt.block, indent + 2);
            break;

        case NODE_WHEN_STMT:
            pad(indent);
            switch (node->as.when_stmt.kind) {
                case WHEN_VMA_CHANGED:
                    printf("WhenStmt CHANGED %s\n", node->as.when_stmt.vma_name);
                    break;
                case WHEN_CONDITION:
                    printf("WhenStmt CONDITION\n");
                    pad(indent + 1); printf("Condition:\n");
                    ast_print(node->as.when_stmt.condition, indent + 2);
                    break;
                case WHEN_PROGRAM_START:
                    printf("WhenStmt PROGRAM_START\n");
                    break;
            }
            pad(indent + 1); printf("Block:\n");
            print_block(node->as.when_stmt.block, indent + 2);
            break;

        case NODE_GOTO_STMT:
            pad(indent); printf("GotoStmt %s\n", node->as.goto_stmt.label);
            break;

        case NODE_LABEL_STMT:
            pad(indent); printf("LabelStmt %s:\n", node->as.label_stmt.label);
            break;

        case NODE_BLOCK:
            pad(indent); printf("Block\n");
            print_block(node, indent + 1);
            break;

        case NODE_PROGRAM:
            pad(indent); printf("Program\n");
            print_block(node, indent + 1);
            break;

        default:
            pad(indent); printf("<unknown node type %d>\n", node->type);
    }
}
