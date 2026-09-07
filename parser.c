#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ---------------------------------------------------------------------
 * Low-level token helpers
 * ------------------------------------------------------------------- */
static char *token_text(Token t) {
    char *s = malloc(t.length + 1);
    memcpy(s, t.start, t.length);
    s[t.length] = '\0';
    return s;
}

static void error_at(Parser *p, Token t, const char *msg) {
    fprintf(stderr, "[line %d] Parse error at '%.*s': %s\n",
            t.line, t.length, t.start, msg);
    p->had_error = 1;
}

static void advance(Parser *p) {
    p->current = p->next;
    p->next = lexer_next_token(&p->lexer);
    /* Surface lexer errors as parse errors rather than silently consuming them */
    while (p->current.type == TOKEN_ERROR) {
        error_at(p, p->current, "lexical error");
        p->current = p->next;
        p->next = lexer_next_token(&p->lexer);
    }
}

static int check(Parser *p, VOTokenType type) { return p->current.type == type; }

static int match(Parser *p, VOTokenType type) {
    if (!check(p, type)) return 0;
    advance(p);
    return 1;
}

static void expect(Parser *p, VOTokenType type, const char *msg) {
    if (check(p, type)) { advance(p); return; }
    error_at(p, p->current, msg);
}

static void skip_newlines(Parser *p) {
    while (check(p, TOKEN_NEWLINE)) advance(p);
}

/* A statement must end at a newline (or EOF, for the final statement in
   a file). This also tolerates blank lines after the statement. */
static void expect_statement_end(Parser *p) {
    if (check(p, TOKEN_EOF)) return;
    if (!check(p, TOKEN_NEWLINE)) {
        error_at(p, p->current, "expected end of line after statement");
        return;
    }
    skip_newlines(p);
}

static int is_assign_op(VOTokenType t) {
    return t == TOKEN_ASSIGN || t == TOKEN_PLUS_ASSIGN || t == TOKEN_MINUS_ASSIGN ||
           t == TOKEN_STAR_ASSIGN || t == TOKEN_SLASH_ASSIGN || t == TOKEN_PERCENT_ASSIGN;
}

static int is_valid_assign_target(ASTNode *n) {
    return n->type == NODE_IDENTIFIER || n->type == NODE_VMA_REF || n->type == NODE_INDEX;
}

/* ---------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------- */
static ASTNode *parse_expression(Parser *p);
static ASTNode *parse_block_until(Parser *p, const VOTokenType *terminators, int n_terminators);
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_conditional(Parser *p, int negate_condition);

/* ---------------------------------------------------------------------
 * Expression grammar (highest precedence number = tightest binding,
 * matching the Virtual Order v1.2 precedence table).
 *
 *   expression   -> assignment
 *   assignment   -> logic_or ( assign_op assignment )?      [right-assoc]
 *   logic_or     -> logic_xor ( OR logic_xor )*
 *   logic_xor    -> logic_and ( XOR logic_and )*
 *   logic_and    -> not_expr ( AND not_expr )*
 *   not_expr     -> NOT not_expr | comparison
 *   comparison   -> bit_or ( (==|!=|<|>|<=|>=) bit_or )*
 *   bit_or       -> bit_xor ( '|' bit_xor )*
 *   bit_xor      -> bit_and ( '^' bit_and )*
 *   bit_and      -> shift ( '&' shift )*
 *   shift        -> additive ( (<<|>>) additive )*
 *   additive     -> multiplicative ( (+|-) multiplicative )*
 *   multiplicative -> unary ( (*|/|%) unary )*
 *   unary        -> '-' unary | power
 *   power        -> primary ( '**' unary )?                 [right-assoc]
 *   primary      -> literals | VMA | IDENTIFIER | LOAD VMA |
 *                   LENGTH '(' expression ')' | '(' expression ')' |
 *                   '[' array_elements ']'
 *                   (each followed by any number of '[' expr ']' index
 *                    postfixes)
 *
 * NOTE (flagged, not in the original spec table): unary minus isn't
 * listed as its own precedence level. It's implemented here as binding
 * tighter than * / % but looser than ** — matching the common
 * convention (e.g. Python) where `-2 ** 2` == `-(2 ** 2)` == -4, not
 * `(-2) ** 2` == 4. Confirm this is the semantics you want; if not,
 * swap the unary/power call order below.
 * ------------------------------------------------------------------- */

static ASTNode *make_binary(VOTokenType op, ASTNode *left, ASTNode *right, int line) {
    ASTNode *n = ast_new(NODE_BINARY, line);
    n->as.binary.op = op;
    n->as.binary.left = left;
    n->as.binary.right = right;
    return n;
}

static ASTNode *parse_array_literal(Parser *p) {
    int line = p->current.line;
    expect(p, TOKEN_LBRACKET, "expected '['");
    ASTNode *n = ast_new(NODE_ARRAY_LITERAL, line);
    nodelist_init(&n->as.array_lit.elements);
    if (!check(p, TOKEN_RBRACKET)) {
        do {
            nodelist_push(&n->as.array_lit.elements, parse_expression(p));
        } while (match(p, TOKEN_COMMA));
    }
    expect(p, TOKEN_RBRACKET, "expected ']' to close array literal");
    return n;
}

static ASTNode *parse_primary(Parser *p) {
    Token t = p->current;

    switch (t.type) {
        case TOKEN_NUM_LITERAL: {
            advance(p);
            ASTNode *n = ast_new(NODE_NUM_LITERAL, t.line);
            char *text = token_text(t);
            n->as.num_lit.value = strtol(text, NULL, 10);
            free(text);
            return n;
        }
        case TOKEN_DEC_LITERAL: {
            advance(p);
            ASTNode *n = ast_new(NODE_DEC_LITERAL, t.line);
            char *text = token_text(t);
            n->as.dec_lit.value = strtod(text, NULL);
            free(text);
            return n;
        }
        case TOKEN_TEX_LITERAL: {
            advance(p);
            /* strip surrounding quotes; leave escapes as-is for now
               (a later lexer/parser pass can unescape if needed) */
            ASTNode *n = ast_new(NODE_TEX_LITERAL, t.line);
            char *raw = token_text(t);
            int len = (int)strlen(raw);
            char *inner = malloc(len - 1);
            memcpy(inner, raw + 1, len - 2);
            inner[len - 2] = '\0';
            n->as.tex_lit.value = inner;
            free(raw);
            return n;
        }
        case TOKEN_YES: {
            advance(p);
            ASTNode *n = ast_new(NODE_BOOL_LITERAL, t.line);
            n->as.bool_lit.value = 1;
            return n;
        }
        case TOKEN_NO: {
            advance(p);
            ASTNode *n = ast_new(NODE_BOOL_LITERAL, t.line);
            n->as.bool_lit.value = 0;
            return n;
        }
        case TOKEN_NULL: {
            advance(p);
            return ast_new(NODE_NULL_LITERAL, t.line);
        }
        case TOKEN_VMA: {
            advance(p);
            ASTNode *n = ast_new(NODE_VMA_REF, t.line);
            n->as.vma_ref.name = token_text(t);
            return n;
        }
        case TOKEN_IDENTIFIER: {
            advance(p);
            ASTNode *n = ast_new(NODE_IDENTIFIER, t.line);
            n->as.identifier.name = token_text(t);
            return n;
        }
        case TOKEN_OLD_VALUE: {
            advance(p);
            ASTNode *n = ast_new(NODE_IDENTIFIER, t.line);
            n->as.identifier.name = token_text(t); /* lexeme is already "OLD_VALUE" */
            return n;
        }
        case TOKEN_NEW_VALUE: {
            advance(p);
            ASTNode *n = ast_new(NODE_IDENTIFIER, t.line);
            n->as.identifier.name = token_text(t); /* lexeme is already "NEW_VALUE" */
            return n;
        }
        case TOKEN_LOAD: {
            advance(p);
            Token vma_tok = p->current;
            expect(p, TOKEN_VMA, "expected a VMA after LOAD");
            ASTNode *vma = ast_new(NODE_VMA_REF, vma_tok.line);
            vma->as.vma_ref.name = token_text(vma_tok);
            ASTNode *n = ast_new(NODE_LOAD, t.line);
            n->as.load.vma = vma;
            return n;
        }
        case TOKEN_LENGTH: {
            advance(p);
            expect(p, TOKEN_LPAREN, "expected '(' after LENGTH");
            ASTNode *arg = parse_expression(p);
            expect(p, TOKEN_RPAREN, "expected ')' after LENGTH argument");
            ASTNode *n = ast_new(NODE_LENGTH_CALL, t.line);
            n->as.length_call.arg = arg;
            return n;
        }
        case TOKEN_LPAREN: {
            advance(p);
            ASTNode *inner = parse_expression(p);
            expect(p, TOKEN_RPAREN, "expected ')' to close grouped expression");
            return inner;
        }
        case TOKEN_LBRACKET:
            return parse_array_literal(p);

        default:
            error_at(p, t, "expected an expression");
            advance(p); /* avoid infinite loop on malformed input */
            return ast_new(NODE_NULL_LITERAL, t.line);
    }
}

/* primary, with postfix `[index]` chaining, e.g. History[I][0] */
static ASTNode *parse_postfix(Parser *p) {
    ASTNode *node = parse_primary(p);
    while (check(p, TOKEN_LBRACKET)) {
        int line = p->current.line;
        advance(p);
        ASTNode *index = parse_expression(p);
        expect(p, TOKEN_RBRACKET, "expected ']' after index expression");
        ASTNode *n = ast_new(NODE_INDEX, line);
        n->as.index_expr.array = node;
        n->as.index_expr.index = index;
        node = n;
    }
    return node;
}

static ASTNode *parse_unary(Parser *p);

static ASTNode *parse_power(Parser *p) {
    ASTNode *left = parse_postfix(p);
    if (check(p, TOKEN_POWER)) {
        int line = p->current.line;
        advance(p);
        ASTNode *right = parse_unary(p); /* right-assoc; allows `2 ** -2` */
        return make_binary(TOKEN_POWER, left, right, line);
    }
    return left;
}

static ASTNode *parse_unary(Parser *p) {
    if (check(p, TOKEN_MINUS)) {
        int line = p->current.line;
        advance(p);
        ASTNode *operand = parse_unary(p);
        ASTNode *n = ast_new(NODE_UNARY, line);
        n->as.unary.op = TOKEN_MINUS;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_power(p);
}

static ASTNode *parse_multiplicative(Parser *p) {
    ASTNode *left = parse_unary(p);
    while (check(p, TOKEN_STAR) || check(p, TOKEN_SLASH) || check(p, TOKEN_PERCENT)) {
        VOTokenType op = p->current.type;
        int line = p->current.line;
        advance(p);
        left = make_binary(op, left, parse_unary(p), line);
    }
    return left;
}

static ASTNode *parse_additive(Parser *p) {
    ASTNode *left = parse_multiplicative(p);
    while (check(p, TOKEN_PLUS) || check(p, TOKEN_MINUS)) {
        VOTokenType op = p->current.type;
        int line = p->current.line;
        advance(p);
        left = make_binary(op, left, parse_multiplicative(p), line);
    }
    return left;
}

static ASTNode *parse_shift(Parser *p) {
    ASTNode *left = parse_additive(p);
    while (check(p, TOKEN_SHL) || check(p, TOKEN_SHR)) {
        VOTokenType op = p->current.type;
        int line = p->current.line;
        advance(p);
        left = make_binary(op, left, parse_additive(p), line);
    }
    return left;
}

static ASTNode *parse_bitand(Parser *p) {
    ASTNode *left = parse_shift(p);
    while (check(p, TOKEN_AMP)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_AMP, left, parse_shift(p), line);
    }
    return left;
}

static ASTNode *parse_bitxor(Parser *p) {
    ASTNode *left = parse_bitand(p);
    while (check(p, TOKEN_CARET)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_CARET, left, parse_bitand(p), line);
    }
    return left;
}

static ASTNode *parse_bitor(Parser *p) {
    ASTNode *left = parse_bitxor(p);
    while (check(p, TOKEN_PIPE)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_PIPE, left, parse_bitxor(p), line);
    }
    return left;
}

static ASTNode *parse_comparison(Parser *p) {
    ASTNode *left = parse_bitor(p);
    while (check(p, TOKEN_EQEQ) || check(p, TOKEN_NEQ) || check(p, TOKEN_LT) ||
           check(p, TOKEN_GT)   || check(p, TOKEN_LE)  || check(p, TOKEN_GE)) {
        VOTokenType op = p->current.type;
        int line = p->current.line;
        advance(p);
        left = make_binary(op, left, parse_bitor(p), line);
    }
    return left;
}

static ASTNode *parse_not(Parser *p) {
    if (check(p, TOKEN_NOT)) {
        int line = p->current.line;
        advance(p);
        ASTNode *operand = parse_not(p); /* right-assoc */
        ASTNode *n = ast_new(NODE_UNARY, line);
        n->as.unary.op = TOKEN_NOT;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_comparison(p);
}

static ASTNode *parse_and(Parser *p) {
    ASTNode *left = parse_not(p);
    while (check(p, TOKEN_AND)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_AND, left, parse_not(p), line);
    }
    return left;
}

static ASTNode *parse_xor(Parser *p) {
    ASTNode *left = parse_and(p);
    while (check(p, TOKEN_XOR)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_XOR, left, parse_and(p), line);
    }
    return left;
}

static ASTNode *parse_or(Parser *p) {
    ASTNode *left = parse_xor(p);
    while (check(p, TOKEN_OR)) {
        int line = p->current.line;
        advance(p);
        left = make_binary(TOKEN_OR, left, parse_xor(p), line);
    }
    return left;
}

static ASTNode *parse_assignment(Parser *p) {
    ASTNode *left = parse_or(p);
    if (is_assign_op(p->current.type)) {
        VOTokenType op = p->current.type;
        int line = p->current.line;
        if (!is_valid_assign_target(left)) {
            error_at(p, p->current, "invalid assignment target");
        }
        advance(p);
        ASTNode *value = parse_assignment(p); /* right-assoc */
        ASTNode *n = ast_new(NODE_ASSIGN, line);
        n->as.assign.op = op;
        n->as.assign.target = left;
        n->as.assign.value = value;
        return n;
    }
    return left;
}

static ASTNode *parse_expression(Parser *p) {
    return parse_assignment(p);
}

/* ---------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------- */

static int type_starts_var_decl(VOTokenType t) {
    return t == TOKEN_TYPE_NUM || t == TOKEN_TYPE_DEC || t == TOKEN_TYPE_TEX ||
           t == TOKEN_TYPE_YN  || t == TOKEN_TYPE_COLL;
}

static ASTNode *parse_var_or_const_decl(Parser *p, int is_const) {
    int line = p->current.line;
    advance(p); /* VAR or CONST */

    if (!type_starts_var_decl(p->current.type)) {
        error_at(p, p->current, "expected a type (NUM/DEC/TEX/YN/COLL)");
    }
    VOTokenType var_type = p->current.type;
    advance(p);

    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected an identifier in declaration");

    expect(p, TOKEN_EAQ, "expected EAQ in declaration");

    ASTNode *init = parse_expression(p);

    ASTNode *n = ast_new(is_const ? NODE_CONST_DECL : NODE_VAR_DECL, line);
    n->as.var_decl.var_type = var_type;
    n->as.var_decl.name = token_text(name_tok);
    n->as.var_decl.init = init;

    expect_statement_end(p);
    return n;
}

/* Identifier/VMA-led statement: assignment, compound assignment, or
   increment/decrement. (Bare expression statements otherwise are not
   part of the grammar - Virtual Order statements are all keyword-led except
   these two forms.) */
static ASTNode *parse_assignment_or_incdec_statement(Parser *p) {
    ASTNode *target = parse_postfix(p); /* identifier / VMA / indexed target */
    int line = p->current.line;

    if (check(p, TOKEN_INCREMENT) || check(p, TOKEN_DECREMENT)) {
        VOTokenType op = p->current.type;
        advance(p);
        if (!is_valid_assign_target(target)) {
            error_at(p, p->current, "invalid target for ++/--");
        }
        ASTNode *n = ast_new(NODE_INC_DEC_STMT, line);
        n->as.inc_dec.target = target;
        n->as.inc_dec.op = op;
        expect_statement_end(p);
        return n;
    }

    if (is_assign_op(p->current.type)) {
        VOTokenType op = p->current.type;
        advance(p);
        if (!is_valid_assign_target(target)) {
            error_at(p, p->current, "invalid assignment target");
        }
        ASTNode *value = parse_expression(p);
        ASTNode *assign = ast_new(NODE_ASSIGN, line);
        assign->as.assign.op = op;
        assign->as.assign.target = target;
        assign->as.assign.value = value;

        ASTNode *stmt = ast_new(NODE_EXPR_STMT, line);
        stmt->as.expr_stmt.expr = assign;
        expect_statement_end(p);
        return stmt;
    }

    error_at(p, p->current, "expected '=', a compound assignment, or '++'/'--' after this");
    expect_statement_end(p);
    ASTNode *stmt = ast_new(NODE_EXPR_STMT, line);
    stmt->as.expr_stmt.expr = target;
    return stmt;
}

static ASTNode *parse_show_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* SHOW */
    ASTNode *expr = parse_expression(p);
    ASTNode *n = ast_new(NODE_SHOW_STMT, line);
    n->as.show_stmt.expr = expr;
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_store_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* STORE */
    ASTNode *value = parse_expression(p);
    Token vma_tok = p->current;
    expect(p, TOKEN_VMA, "expected a VMA as the STORE target");
    ASTNode *n = ast_new(NODE_STORE_STMT, line);
    n->as.store_stmt.value = value;
    n->as.store_stmt.target_vma = token_text(vma_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_clean_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* CLEAN */
    Token target_tok = p->current;
    if (!check(p, TOKEN_VMA) && !check(p, TOKEN_IDENTIFIER)) {
        error_at(p, p->current, "expected a VMA or identifier after CLEAN");
    }
    advance(p);
    ASTNode *n = ast_new(NODE_CLEAN_STMT, line);
    n->as.clean_stmt.target = token_text(target_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_cleanall_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* CLEANALL */
    ASTNode *n = ast_new(NODE_CLEANALL_STMT, line);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_autoclean_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* AUTOCLEAN */
    int on;
    if (match(p, TOKEN_ON)) on = 1;
    else if (match(p, TOKEN_OFF)) on = 0;
    else { error_at(p, p->current, "expected ON or OFF after AUTOCLEAN"); on = 0; }
    ASTNode *n = ast_new(NODE_AUTOCLEAN_STMT, line);
    n->as.autoclean_stmt.on = on;
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_goto_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* GOTO */
    Token label_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected a label name after GOTO");
    ASTNode *n = ast_new(NODE_GOTO_STMT, line);
    n->as.goto_stmt.label = token_text(label_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_label_stmt(Parser *p) {
    int line = p->current.line;
    Token label_tok = p->current;
    advance(p); /* IDENTIFIER */
    advance(p); /* ':' */
    ASTNode *n = ast_new(NODE_LABEL_STMT, line);
    n->as.label_stmt.label = token_text(label_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_if_branch(Parser *p, ASTNode *condition, const VOTokenType *terms, int n_terms) {
    int line = p->current.line;
    ASTNode *block = parse_block_until(p, terms, n_terms);
    ASTNode *branch = ast_new(NODE_IF_BRANCH, line);
    branch->as.if_branch.condition = condition;
    branch->as.if_branch.block = block;
    return branch;
}

/* Handles both:
 *   IF cond ... (ORIF cond ...)* (IFNOT [cond] ...)? ENDIF
 *   IFNOT cond ... (ORIF cond ...)* (IFNOT [cond] ...)? ENDIF   (== IF NOT cond ...)
 */
static ASTNode *parse_conditional(Parser *p, int negate_condition) {
    int line = p->current.line;
    advance(p); /* IF or IFNOT */

    ASTNode *condition = parse_expression(p);
    if (negate_condition) {
        ASTNode *n = ast_new(NODE_UNARY, line);
        n->as.unary.op = TOKEN_NOT;
        n->as.unary.operand = condition;
        condition = n;
    }
    expect_statement_end(p);

    ASTNode *if_stmt = ast_new(NODE_IF_STMT, line);
    nodelist_init(&if_stmt->as.if_stmt.branches);

    VOTokenType terms[] = { TOKEN_ORIF, TOKEN_IFNOT, TOKEN_ENDIF };
    nodelist_push(&if_stmt->as.if_stmt.branches,
                  parse_if_branch(p, condition, terms, 3));

    while (check(p, TOKEN_ORIF)) {
        int bline = p->current.line;
        advance(p);
        ASTNode *cond = parse_expression(p);
        expect_statement_end(p);
        ASTNode *block = parse_block_until(p, terms, 3);
        ASTNode *branch = ast_new(NODE_IF_BRANCH, bline);
        branch->as.if_branch.condition = cond;
        branch->as.if_branch.block = block;
        nodelist_push(&if_stmt->as.if_stmt.branches, branch);
    }

    if (check(p, TOKEN_IFNOT)) {
        int bline = p->current.line;
        advance(p);
        ASTNode *cond = NULL;
        if (!check(p, TOKEN_NEWLINE)) {
            /* IFNOT with a trailing condition here reads as another
               conditional branch (== ORIF NOT cond), not just else. */
            ASTNode *raw = parse_expression(p);
            cond = ast_new(NODE_UNARY, bline);
            cond->as.unary.op = TOKEN_NOT;
            cond->as.unary.operand = raw;
        }
        expect_statement_end(p);
        VOTokenType only_endif[] = { TOKEN_ENDIF };
        ASTNode *block = parse_block_until(p, only_endif, 1);
        ASTNode *branch = ast_new(NODE_IF_BRANCH, bline);
        branch->as.if_branch.condition = cond; /* NULL => plain else */
        branch->as.if_branch.block = block;
        nodelist_push(&if_stmt->as.if_stmt.branches, branch);
    }

    expect(p, TOKEN_ENDIF, "expected ENDIF to close this IF chain");
    expect_statement_end(p);
    return if_stmt;
}

static ASTNode *parse_while_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* WHILE */
    ASTNode *condition = parse_expression(p);
    expect_statement_end(p);
    VOTokenType terms[] = { TOKEN_ENDWHILE };
    ASTNode *block = parse_block_until(p, terms, 1);
    expect(p, TOKEN_ENDWHILE, "expected ENDWHILE");
    expect_statement_end(p);

    ASTNode *n = ast_new(NODE_WHILE_STMT, line);
    n->as.while_stmt.condition = condition;
    n->as.while_stmt.block = block;
    return n;
}

static ASTNode *parse_for_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* FOR */
    Token iter_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected loop variable name after FOR");
    expect(p, TOKEN_ASSIGN, "expected '=' after FOR loop variable");
    ASTNode *start = parse_expression(p);
    expect(p, TOKEN_TO, "expected TO in FOR loop range");
    ASTNode *end = parse_expression(p);
    expect_statement_end(p);
    VOTokenType terms[] = { TOKEN_ENDFOR };
    ASTNode *block = parse_block_until(p, terms, 1);
    expect(p, TOKEN_ENDFOR, "expected ENDFOR");
    expect_statement_end(p);

    ASTNode *n = ast_new(NODE_FOR_STMT, line);
    n->as.for_stmt.iterator = token_text(iter_tok);
    n->as.for_stmt.start = start;
    n->as.for_stmt.end = end;
    n->as.for_stmt.block = block;
    return n;
}

static ASTNode *parse_when_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* WHEN */

    ASTNode *n = ast_new(NODE_WHEN_STMT, line);

    if (check(p, TOKEN_PROGRAM)) {
        advance(p);
        expect(p, TOKEN_START, "expected START after WHEN PROGRAM");
        n->as.when_stmt.kind = WHEN_PROGRAM_START;
    } else if (check(p, TOKEN_VMA) && p->next.type == TOKEN_CHANGED) {
        Token vma_tok = p->current;
        advance(p); /* VMA */
        advance(p); /* CHANGED */
        n->as.when_stmt.kind = WHEN_VMA_CHANGED;
        n->as.when_stmt.vma_name = token_text(vma_tok);
    } else {
        n->as.when_stmt.kind = WHEN_CONDITION;
        n->as.when_stmt.condition = parse_expression(p);
    }

    expect_statement_end(p);
    VOTokenType terms[] = { TOKEN_ENDWHEN };
    n->as.when_stmt.block = parse_block_until(p, terms, 1);
    expect(p, TOKEN_ENDWHEN, "expected ENDWHEN");
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_statement(Parser *p) {
    switch (p->current.type) {
        case TOKEN_VAR:       return parse_var_or_const_decl(p, 0);
        case TOKEN_CONST:     return parse_var_or_const_decl(p, 1);
        case TOKEN_SHOW:      return parse_show_stmt(p);
        case TOKEN_STORE:     return parse_store_stmt(p);
        case TOKEN_CLEAN:     return parse_clean_stmt(p);
        case TOKEN_CLEANALL:  return parse_cleanall_stmt(p);
        case TOKEN_AUTOCLEAN: return parse_autoclean_stmt(p);
        case TOKEN_GOTO:      return parse_goto_stmt(p);
        case TOKEN_IF:        return parse_conditional(p, 0);
        case TOKEN_WHILE:     return parse_while_stmt(p);
        case TOKEN_FOR:       return parse_for_stmt(p);
        case TOKEN_WHEN:      return parse_when_stmt(p);

        case TOKEN_IFNOT:
            /* IFNOT with a condition on its own is IF-NOT shorthand;
               bare IFNOT can only appear as an else-branch inside an
               existing chain, which parse_conditional's caller context
               (not parse_statement) is responsible for reaching. */
            if (p->next.type == TOKEN_NEWLINE) {
                error_at(p, p->current,
                          "bare IFNOT (else-branch) may only appear inside an IF...ENDIF chain");
            }
            return parse_conditional(p, 1);

        case TOKEN_IDENTIFIER:
            if (p->next.type == TOKEN_COLON) return parse_label_stmt(p);
            return parse_assignment_or_incdec_statement(p);

        case TOKEN_VMA:
            return parse_assignment_or_incdec_statement(p);

        default:
            error_at(p, p->current, "expected a statement");
            advance(p);
            return NULL;
    }
}

/* Parses statements until one of the given terminator token types is
   seen (without consuming it), returning them wrapped in a NODE_BLOCK. */
static ASTNode *parse_block_until(Parser *p, const VOTokenType *terminators, int n_terminators) {
    ASTNode *block = ast_new(NODE_BLOCK, p->current.line);
    nodelist_init(&block->as.block.statements);
    skip_newlines(p);

    for (;;) {
        if (check(p, TOKEN_EOF)) break;
        int stop = 0;
        for (int i = 0; i < n_terminators; i++) {
            if (check(p, terminators[i])) { stop = 1; break; }
        }
        if (stop) break;

        ASTNode *stmt = parse_statement(p);
        if (stmt) nodelist_push(&block->as.block.statements, stmt);
        skip_newlines(p);

        if (p->had_error) {
            /* best-effort recovery: bail out of this block rather than
               looping forever if something is badly malformed */
            break;
        }
    }
    return block;
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */
void parser_init(Parser *p, const char *source) {
    lexer_init(&p->lexer, source);
    p->had_error = 0;
    p->current = lexer_next_token(&p->lexer);
    p->next = lexer_next_token(&p->lexer);
    while (p->current.type == TOKEN_ERROR) {
        error_at(p, p->current, "lexical error");
        p->current = p->next;
        p->next = lexer_next_token(&p->lexer);
    }
}

ASTNode *parser_parse_program(Parser *p) {
    ASTNode *program = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&program->as.block.statements);
    skip_newlines(p);

    while (!check(p, TOKEN_EOF)) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) nodelist_push(&program->as.block.statements, stmt);
        skip_newlines(p);
    }
    return program;
}
