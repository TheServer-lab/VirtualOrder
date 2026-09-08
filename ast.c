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
        case NODE_EMP_LITERAL:
            pad(indent); printf("EmpLiteral\n");
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
        case NODE_SLICE:
            pad(indent); printf("Slice\n");
            ast_print(node->as.slice_expr.array, indent + 1);
            if (node->as.slice_expr.start) ast_print(node->as.slice_expr.start, indent + 1);
            else { pad(indent + 1); printf("(start omitted)\n"); }
            if (node->as.slice_expr.end) ast_print(node->as.slice_expr.end, indent + 1);
            else { pad(indent + 1); printf("(end omitted)\n"); }
            break;
        case NODE_TEX_INTERP:
            pad(indent); printf("TexInterp\n");
            for (int i = 0; i < node->as.tex_interp.parts.count; i++)
                ast_print(node->as.tex_interp.parts.items[i], indent + 1);
            break;
        case NODE_INTERP_LITERAL:
            pad(indent); printf("InterpLiteral \"%s\"\n", node->as.interp_lit.text);
            break;
        case NODE_COMMAND:
            pad(indent); printf("Command kind=%d\n", node->as.command.kind);
            for (int i = 0; i < node->as.command.args.count; i++)
                ast_print(node->as.command.args.items[i], indent + 1);
            break;
        case NODE_CALL:
            pad(indent); printf("Call\n");
            ast_print(node->as.call.callee, indent + 1);
            for (int i = 0; i < node->as.call.args.count; i++)
                ast_print(node->as.call.args.items[i], indent + 1);
            break;
        case NODE_MODULE_REF:
            pad(indent); printf("ModuleRef %s.%s\n",
                   node->as.module_ref.module, node->as.module_ref.member);
            break;

        case NODE_VAR_DECL:
            pad(indent);
            printf("VarDecl %s %s\n",
                   token_type_name(node->as.var_decl.var_type),
                   node->as.var_decl.name);
            ast_print(node->as.var_decl.init, indent + 1);
            break;

        case NODE_HARD_DECL:
            pad(indent);
            printf("HardDecl %s %s\n",
                   token_type_name(node->as.hard_decl.var_type),
                   node->as.hard_decl.name);
            ast_print(node->as.hard_decl.value, indent + 1);
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
            for (int i = 0; i < node->as.show_stmt.expr.count; i++)
                ast_print(node->as.show_stmt.expr.items[i], indent + 1);
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

        case NODE_GIVE_STMT:
            pad(indent); printf("GiveStmt\n");
            ast_print(node->as.give_stmt.value, indent + 1);
            break;

        case NODE_DEMAND_STMT:
            pad(indent); printf("DemandStmt");
            if (node->as.demand_stmt.message)
                printf(" \"%s\"", node->as.demand_stmt.message);
            printf("\n");
            ast_print(node->as.demand_stmt.condition, indent + 1);
            break;

        case NODE_SERVE_STMT:
            pad(indent); printf("ServeStmt\n");
            ast_print(node->as.serve_stmt.value, indent + 1);
            break;

        case NODE_DO_STMT:
            pad(indent); printf("DoStmt\n");
            pad(indent + 1); printf("Try:\n");
            print_block(node->as.do_stmt.try_block, indent + 2);
            pad(indent + 1); printf("Grabe:\n");
            print_block(node->as.do_stmt.catch_block, indent + 2);
            break;

        case NODE_JOB_DECL:
            pad(indent); printf("JobDecl %s (%d params)\n",
                   node->as.job_decl.name, node->as.job_decl.param_count);
            for (int i = 0; i < node->as.job_decl.param_count; i++)
                { pad(indent + 1); printf("Param: %s %s\n",
                     token_type_name(node->as.job_decl.params[i].var_type),
                     node->as.job_decl.params[i].name); }
            pad(indent + 1); printf("Body:\n");
            print_block(node->as.job_decl.body, indent + 2);
            break;

        case NODE_PEICE_DECL:
            pad(indent); printf("PeiceDecl %s\n", node->as.peice_decl.name);
            print_block(node->as.peice_decl.body, indent + 1);
            break;

        case NODE_BRING_STMT:
            pad(indent); printf("BringStmt %s\n", node->as.bring_stmt.path);
            break;

        case NODE_SHIP_STMT:
            pad(indent); printf("ShipStmt %s\n", node->as.ship_stmt.name);
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

void ast_free(ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_TEX_LITERAL:
            free(node->as.tex_lit.value);
            break;
        case NODE_VMA_REF:
            free(node->as.vma_ref.name);
            break;
        case NODE_IDENTIFIER:
            free(node->as.identifier.name);
            break;
        case NODE_UNARY:
            ast_free(node->as.unary.operand);
            break;
        case NODE_BINARY:
            ast_free(node->as.binary.left);
            ast_free(node->as.binary.right);
            break;
        case NODE_ASSIGN:
            ast_free(node->as.assign.target);
            ast_free(node->as.assign.value);
            break;
        case NODE_LOAD:
            ast_free(node->as.load.vma);
            break;
        case NODE_LENGTH_CALL:
            ast_free(node->as.length_call.arg);
            break;
        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                ast_free(node->as.array_lit.elements.items[i]);
            free(node->as.array_lit.elements.items);
            break;
        case NODE_INDEX:
            ast_free(node->as.index_expr.array);
            ast_free(node->as.index_expr.index);
            break;
        case NODE_SLICE:
            ast_free(node->as.slice_expr.array);
            ast_free(node->as.slice_expr.start);
            ast_free(node->as.slice_expr.end);
            break;
        case NODE_TEX_INTERP:
            for (int i = 0; i < node->as.tex_interp.parts.count; i++)
                ast_free(node->as.tex_interp.parts.items[i]);
            free(node->as.tex_interp.parts.items);
            break;
        case NODE_INTERP_LITERAL:
            free(node->as.interp_lit.text);
            break;
        case NODE_COMMAND:
            for (int i = 0; i < node->as.command.args.count; i++)
                ast_free(node->as.command.args.items[i]);
            free(node->as.command.args.items);
            break;
        case NODE_CALL:
            ast_free(node->as.call.callee);
            for (int i = 0; i < node->as.call.args.count; i++)
                ast_free(node->as.call.args.items[i]);
            free(node->as.call.args.items);
            break;
        case NODE_MODULE_REF:
            free(node->as.module_ref.module);
            free(node->as.module_ref.member);
            break;
        case NODE_VAR_DECL:
            free(node->as.var_decl.name);
            ast_free(node->as.var_decl.init);
            break;
        case NODE_HARD_DECL:
            free(node->as.hard_decl.name);
            ast_free(node->as.hard_decl.value);
            break;
        case NODE_EXPR_STMT:
            ast_free(node->as.expr_stmt.expr);
            break;
        case NODE_INC_DEC_STMT:
            ast_free(node->as.inc_dec.target);
            break;
        case NODE_SHOW_STMT:
            for (int i = 0; i < node->as.show_stmt.expr.count; i++)
                ast_free(node->as.show_stmt.expr.items[i]);
            free(node->as.show_stmt.expr.items);
            break;
        case NODE_STORE_STMT:
            ast_free(node->as.store_stmt.value);
            free(node->as.store_stmt.target_vma);
            break;
        case NODE_CLEAN_STMT:
            free(node->as.clean_stmt.target);
            break;
        case NODE_IF_BRANCH:
            ast_free(node->as.if_branch.condition);
            ast_free(node->as.if_branch.block);
            break;
        case NODE_IF_STMT:
            for (int i = 0; i < node->as.if_stmt.branches.count; i++)
                ast_free(node->as.if_stmt.branches.items[i]);
            free(node->as.if_stmt.branches.items);
            break;
        case NODE_WHILE_STMT:
            ast_free(node->as.while_stmt.condition);
            ast_free(node->as.while_stmt.block);
            break;
        case NODE_FOR_STMT:
            free(node->as.for_stmt.iterator);
            ast_free(node->as.for_stmt.start);
            ast_free(node->as.for_stmt.end);
            ast_free(node->as.for_stmt.block);
            break;
        case NODE_WHEN_STMT:
            free(node->as.when_stmt.vma_name);
            ast_free(node->as.when_stmt.condition);
            ast_free(node->as.when_stmt.block);
            break;
        case NODE_GOTO_STMT:
            free(node->as.goto_stmt.label);
            break;
        case NODE_LABEL_STMT:
            free(node->as.label_stmt.label);
            break;
        case NODE_GIVE_STMT:
            ast_free(node->as.give_stmt.value);
            break;
        case NODE_DEMAND_STMT:
            ast_free(node->as.demand_stmt.condition);
            free(node->as.demand_stmt.message);
            break;
        case NODE_SERVE_STMT:
            ast_free(node->as.serve_stmt.value);
            break;
        case NODE_DO_STMT:
            ast_free(node->as.do_stmt.try_block);
            ast_free(node->as.do_stmt.catch_block);
            break;
        case NODE_BRING_STMT:
            free(node->as.bring_stmt.path);
            break;
        case NODE_SHIP_STMT:
            free(node->as.ship_stmt.name);
            break;
        case NODE_JOB_DECL:
            free(node->as.job_decl.name);
            for (int i = 0; i < node->as.job_decl.param_count; i++) {
                free(node->as.job_decl.params[i].name);
            }
            free(node->as.job_decl.params);
            ast_free(node->as.job_decl.body);
            break;
        case NODE_PEICE_DECL:
            free(node->as.peice_decl.name);
            ast_free(node->as.peice_decl.body);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.statements.count; i++)
                ast_free(node->as.block.statements.items[i]);
            free(node->as.block.statements.items);
            break;
        case NODE_PROGRAM:
            ast_free(node->as.block.statements.items[0]);
            free(node->as.block.statements.items);
            break;
        default:
            break;
    }
    free(node);
}
