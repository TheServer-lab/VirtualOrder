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
            t.line, t.length, t.start ? t.start : "", msg);
    p->had_error = 1;
}

static void error_msg(Parser *p, int line, const char *msg) {
    fprintf(stderr, "[line %d] Parse error: %s\n", line, msg);
    p->had_error = 1;
}

static void advance(Parser *p) {
    p->current = p->next;
    p->next = lexer_next_token(&p->lexer);
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
    return n->type == NODE_IDENTIFIER || n->type == NODE_VMA_REF || n->type == NODE_INDEX
        || n->type == NODE_MODULE_REF;
}

/* ---------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------- */
static ASTNode *parse_expression(Parser *p);
static ASTNode *parse_block_until(Parser *p, const VOTokenType *terminators, int n_terminators);
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_conditional(Parser *p, int negate_condition);
static ASTNode *parse_command_call(Parser *p, CommandKind kind);
static ASTNode *parse_command_stmt(Parser *p, CommandKind kind);
static ASTNode *parse_command_operand(Parser *p, CommandKind kind);

/* ---------------------------------------------------------------------
 * Type keyword check (for VAR/HARD/JOB params)
 * ------------------------------------------------------------------- */
static int is_type_keyword(VOTokenType t) {
    return t == TOKEN_TYPE_NUM || t == TOKEN_TYPE_DEC || t == TOKEN_TYPE_TEX ||
           t == TOKEN_TYPE_YN  || t == TOKEN_TYPE_COLL || t == TOKEN_TYPE_EMP ||
           t == TOKEN_TYPE_FILE || t == TOKEN_TYPE_TASK ||
           t == TOKEN_TYPE_LOCK || t == TOKEN_TYPE_EVENT;
}

/* ---------------------------------------------------------------------
 * Expression grammar
 * ------------------------------------------------------------------- */

static ASTNode *make_binary(VOTokenType op, ASTNode *left, ASTNode *right, int line) {
    ASTNode *n = ast_new(NODE_BINARY, line);
    n->as.binary.op = op;
    n->as.binary.left = left;
    n->as.binary.right = right;
    return n;
}

/* Build a NODE_TEX_INTERP expression from the inner content of a TEX literal.
   The `{expr}` regions are parsed as real expressions by spinning up a
   fresh Parser on each sub-expression's source slice. `{{` and `}}` are
   literal braces. A stray, unpaired `}` is a parse error. */
static ASTNode *tex_interp_from_inner(Parser *p, const char *inner, int line) {
    ASTNode *n = ast_new(NODE_TEX_INTERP, line);
    nodelist_init(&n->as.tex_interp.parts);

    int len = (int)strlen(inner);
    char *lit = malloc(len * 2 + 2);
    int lit_len = 0;

#define EMIT_LIT()                                                        \
    do {                                                                  \
        if (lit_len > 0) {                                                \
            ASTNode *ln = ast_new(NODE_INTERP_LITERAL, line);             \
            ln->as.interp_lit.text = malloc(lit_len + 1);                 \
            memcpy(ln->as.interp_lit.text, lit, lit_len);                 \
            ln->as.interp_lit.text[lit_len] = '\0';                       \
            nodelist_push(&n->as.tex_interp.parts, ln);                   \
            lit_len = 0;                                                  \
        }                                                                 \
    } while (0)

    int i = 0;
    while (i < len) {
        char c = inner[i];
        if (c == '{') {
            if (i + 1 < len && inner[i + 1] == '{') {
                lit[lit_len++] = '{';
                i += 2;
                continue;
            }
            /* find closing '}' */
            int j = i + 1;
            while (j < len && inner[j] != '}') {
                if (inner[j] == '{') {
                    /* nested/unclosed brace inside interpolation is malformed */
                    error_msg(p, line, "malformed interpolation: missing '}'");
                    free(n->as.tex_interp.parts.items);
                    nodelist_init(&n->as.tex_interp.parts);
                    free(lit);
                    ast_free(n);
                    return ast_new(NODE_EMP_LITERAL, line);
                }
                j++;
            }
            if (j >= len) {
                error_msg(p, line, "malformed interpolation: missing '}'");
                free(n->as.tex_interp.parts.items);
                nodelist_init(&n->as.tex_interp.parts);
                free(lit);
                ast_free(n);
                return ast_new(NODE_EMP_LITERAL, line);
            }
            /* literal text so far becomes an INTERP_LITERAL part */
            EMIT_LIT();
            /* sub-expression is inner[i+1 .. j-1] */
            int sub_len = j - (i + 1);
            char *sub = malloc(sub_len + 1);
            if (sub_len > 0) {
                memcpy(sub, inner + i + 1, sub_len);
                sub[sub_len] = '\0';
                Parser sp;
                parser_init(&sp, sub);
                ASTNode *expr = parse_expression(&sp);
                int ok = !sp.had_error && check(&sp, TOKEN_EOF);
                free(sub);
                if (!ok) {
                    error_msg(p, line, "malformed interpolation expression");
                    if (expr) ast_free(expr);
                    free(n->as.tex_interp.parts.items);
                    nodelist_init(&n->as.tex_interp.parts);
                    free(lit);
                    ast_free(n);
                    return ast_new(NODE_EMP_LITERAL, line);
                }
                nodelist_push(&n->as.tex_interp.parts, expr);
            } else {
                free(sub);
                error_msg(p, line, "empty interpolation expression");
                free(n->as.tex_interp.parts.items);
                nodelist_init(&n->as.tex_interp.parts);
                free(lit);
                ast_free(n);
                return ast_new(NODE_EMP_LITERAL, line);
            }
            i = j + 1;
        } else if (c == '}') {
            if (i + 1 < len && inner[i + 1] == '}') {
                lit[lit_len++] = '}';
                i += 2;
                continue;
            }
            error_msg(p, line, "malformed interpolation: unexpected '}'");
            free(n->as.tex_interp.parts.items);
            nodelist_init(&n->as.tex_interp.parts);
            free(lit);
            ast_free(n);
            return ast_new(NODE_EMP_LITERAL, line);
            i++;
        } else {
            lit[lit_len++] = c;
            i++;
        }
    }

    EMIT_LIT();
#undef EMIT_LIT
    free(lit);
    return n;
}

static ASTNode *parse_tex_literal(Parser *p) {
    Token t = p->current;
    advance(p);

    char *raw = token_text(t);
    int len = (int)strlen(raw);
    char *inner = malloc(len - 1);
    memcpy(inner, raw + 1, len - 2);
    inner[len - 2] = '\0';
    free(raw);

    if (!strchr(inner, '{') && !strchr(inner, '}')) {
        ASTNode *n = ast_new(NODE_TEX_LITERAL, t.line);
        n->as.tex_lit.value = inner;
        return n;
    }

    ASTNode *n = tex_interp_from_inner(p, inner, t.line);
    free(inner);
    return n;
}

/* Expression-form builtin command: NAME(args...) */
static ASTNode *parse_command_call(Parser *p, CommandKind kind) {
    int line = p->current.line;
    advance(p); /* command keyword */
    expect(p, TOKEN_LPAREN, "expected '(' after command");
    ASTNode *n = ast_new(NODE_COMMAND, line);
    n->as.command.kind = kind;
    nodelist_init(&n->as.command.args);
    if (!check(p, TOKEN_RPAREN)) {
        do {
            nodelist_push(&n->as.command.args, parse_expression(p));
        } while (match(p, TOKEN_COMMA));
    }
    expect(p, TOKEN_RPAREN, "expected ')' after command arguments");
    return n;
}

/* Operand-form command used in expression positions (SPAWN work(), CLAIM W):
   the operand is one or more unbracketed expressions, e.g. SPAWN JOB call. */
static ASTNode *parse_command_operand(Parser *p, CommandKind kind) {
    int line = p->current.line;
    advance(p); /* command keyword */
    ASTNode *n = ast_new(NODE_COMMAND, line);
    n->as.command.kind = kind;
    nodelist_init(&n->as.command.args);
    if (!check(p, TOKEN_NEWLINE) && !check(p, TOKEN_EOF) && !check(p, TOKEN_COMMA)) {
        do {
            nodelist_push(&n->as.command.args, parse_expression(p));
        } while (match(p, TOKEN_COMMA));
    }
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
            return parse_tex_literal(p);
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
        case TOKEN_EMP:
        case TOKEN_TYPE_EMP: {
            advance(p);
            return ast_new(NODE_EMP_LITERAL, t.line);
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
            n->as.identifier.name = token_text(t);
            return n;
        }
        case TOKEN_NEW_VALUE: {
            advance(p);
            ASTNode *n = ast_new(NODE_IDENTIFIER, t.line);
            n->as.identifier.name = token_text(t);
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

        /* Expression-form commands (parenthesized): COLL/TEX, files  */
        case TOKEN_COUNT:    return parse_command_call(p, CMD_COUNT);
        case TOKEN_TAKE:     return parse_command_call(p, CMD_TAKE);
        case TOKEN_SEEK:     return parse_command_call(p, CMD_SEEK);
        case TOKEN_HAS:      return parse_command_call(p, CMD_HAS);
        case TOKEN_BIND:     return parse_command_call(p, CMD_BIND);
        case TOKEN_SEVER:    return parse_command_call(p, CMD_SEVER);
        case TOKEN_CUT:      return parse_command_call(p, CMD_CUT);
        case TOKEN_RAISE:    return parse_command_call(p, CMD_RAISE);
        case TOKEN_LOWER:    return parse_command_call(p, CMD_LOWER);
        case TOKEN_DRAW:     return parse_command_call(p, CMD_DRAW);
        case TOKEN_UNSEAL:   return parse_command_call(p, CMD_UNSEAL);
        case TOKEN_SPAWN:    return parse_command_operand(p, CMD_SPAWN);
        case TOKEN_CLAIM:    return parse_command_operand(p, CMD_CLAIM);

        default:
            error_at(p, t, "expected an expression");
            advance(p);
            return ast_new(NODE_EMP_LITERAL, t.line);
    }
}

/* Primary, with postfix [index], (call), and .member chaining */
static ASTNode *parse_postfix(Parser *p) {
    ASTNode *node = parse_primary(p);
    for (;;) {
        if (check(p, TOKEN_LBRACKET)) {
            int line = p->current.line;
            advance(p);
            ASTNode *start = NULL;
            ASTNode *end = NULL;
            if (!check(p, TOKEN_COLON)) {
                start = parse_expression(p);
            }
            if (match(p, TOKEN_COLON)) {
                /* slice form: [start:end] with either bound optional */
                if (!check(p, TOKEN_RBRACKET))
                    end = parse_expression(p);
                expect(p, TOKEN_RBRACKET, "expected ']' after slice expression");
                ASTNode *n = ast_new(NODE_SLICE, line);
                n->as.slice_expr.array = node;
                n->as.slice_expr.start = start;
                n->as.slice_expr.end = end;
                node = n;
            } else {
                /* plain index form */
                if (!start) {
                    error_at(p, p->current, "expected index expression after '['");
                }
                expect(p, TOKEN_RBRACKET, "expected ']' after index expression");
                ASTNode *n = ast_new(NODE_INDEX, line);
                n->as.index_expr.array = node;
                n->as.index_expr.index = start;
                node = n;
            }
        } else if (check(p, TOKEN_LPAREN)) {
            /* Function call */
            int line = p->current.line;
            advance(p);
            ASTNode *n = ast_new(NODE_CALL, line);
            n->as.call.callee = node;
            nodelist_init(&n->as.call.args);
            if (!check(p, TOKEN_RPAREN)) {
                do {
                    nodelist_push(&n->as.call.args, parse_expression(p));
                } while (match(p, TOKEN_COMMA));
            }
            expect(p, TOKEN_RPAREN, "expected ')' after call arguments");
            node = n;
        } else if (check(p, TOKEN_DOT)) {
            int line = p->current.line;
            advance(p);
            if (!check(p, TOKEN_IDENTIFIER)) {
                error_at(p, p->current, "expected member name after '.'");
                expect(p, TOKEN_IDENTIFIER, "expected identifier");
            }
            /* Build a MODULE_REF wrapping the current node */
            ASTNode *n = ast_new(NODE_MODULE_REF, line);
            /* Extract the left side's name - either an identifier or nested module ref */
            if (node->type == NODE_IDENTIFIER) {
                n->as.module_ref.module = node->as.identifier.name;
                n->as.module_ref.member = token_text(p->current);
            } else if (node->type == NODE_MODULE_REF) {
                /* e.g., a.b.c - flatten to module="a.b", member="c" */
                size_t mlen = strlen(node->as.module_ref.module) + 1 + strlen(node->as.module_ref.member);
                char *combined = malloc(mlen + 1);
                snprintf(combined, mlen + 1, "%s.%s", node->as.module_ref.module, node->as.module_ref.member);
                n->as.module_ref.module = combined;
                n->as.module_ref.member = token_text(p->current);
            } else {
                error_at(p, p->current, "invalid left side of '.'");
                n->as.module_ref.module = strdup("?");
                n->as.module_ref.member = token_text(p->current);
            }
            advance(p);
            node = n;
        } else {
            break;
        }
    }
    return node;
}

static ASTNode *parse_unary(Parser *p);

static ASTNode *parse_power(Parser *p) {
    ASTNode *left = parse_postfix(p);
    if (check(p, TOKEN_POWER)) {
        int line = p->current.line;
        advance(p);
        ASTNode *right = parse_unary(p);
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
        ASTNode *operand = parse_not(p);
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
        ASTNode *value = parse_assignment(p);
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
 * JOB parameter list: TYPE IDENTIFIER [TYPE IDENTIFIER ...]
 * No commas, no parentheses around the param list.
 * ------------------------------------------------------------------- */
static JobParam *parse_job_params(Parser *p, int *out_count) {
    JobParam *params = NULL;
    int count = 0;
    int cap = 0;

    while (is_type_keyword(p->current.type)) {
        if (count == cap) {
            cap = cap ? cap * 2 : 4;
            params = realloc(params, sizeof(JobParam) * cap);
        }
        params[count].var_type = p->current.type;
        advance(p);
        if (!check(p, TOKEN_IDENTIFIER)) {
            error_at(p, p->current, "expected parameter name after type");
            advance(p);
        }
        params[count].name = token_text(p->current);
        advance(p);
        count++;
    }
    *out_count = count;
    return params;
}

/* ---------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------- */

static ASTNode *parse_var_decl(Parser *p) {
    int line = p->current.line;
    advance(p); /* VAR */

    if (!is_type_keyword(p->current.type)) {
        error_at(p, p->current, "expected a type (NUM/DEC/TEX/YN/COLL/EMP/FILE/TASK/LOCK/EVENT)");
    }
    VOTokenType var_type = p->current.type;
    advance(p);

    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected an identifier in declaration");
    expect(p, TOKEN_EAQ, "expected EAQ in declaration");

    ASTNode *init = parse_expression(p);

    ASTNode *n = ast_new(NODE_VAR_DECL, line);
    n->as.var_decl.var_type = var_type;
    n->as.var_decl.name = token_text(name_tok);
    n->as.var_decl.init = init;

    expect_statement_end(p);
    return n;
}

static ASTNode *parse_hard_decl(Parser *p) {
    int line = p->current.line;
    advance(p); /* HARD */

    if (!is_type_keyword(p->current.type)) {
        error_at(p, p->current, "expected a type (NUM/DEC/TEX/YN/COLL/EMP/FILE/TASK/LOCK/EVENT)");
    }
    VOTokenType var_type = p->current.type;
    advance(p);

    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected a name in HARD declaration");

    expect(p, TOKEN_ASSIGN, "expected '=' in HARD declaration");

    ASTNode *value = parse_expression(p);

    ASTNode *n = ast_new(NODE_HARD_DECL, line);
    n->as.hard_decl.var_type = var_type;
    n->as.hard_decl.name = token_text(name_tok);
    n->as.hard_decl.value = value;

    expect_statement_end(p);
    return n;
}

/* Identifier/VMA/module-led statement: assignment, compound assignment,
   increment/decrement, or expression statement (function call). */
static ASTNode *parse_assignment_or_incdec_statement(Parser *p) {
    ASTNode *target = parse_postfix(p);
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

    /* If the postfix already produced a call expression, use it as an expr stmt */
    if (target->type == NODE_CALL || target->type == NODE_MODULE_REF) {
        ASTNode *stmt = ast_new(NODE_EXPR_STMT, line);
        stmt->as.expr_stmt.expr = target;
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
    advance(p);
    ASTNode *n = ast_new(NODE_SHOW_STMT, line);
    nodelist_init(&n->as.show_stmt.expr);
    do {
        nodelist_push(&n->as.show_stmt.expr, parse_expression(p));
    } while (match(p, TOKEN_COMMA));
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_store_stmt(Parser *p) {
    int line = p->current.line;
    advance(p);
    ASTNode *value = parse_expression(p);
    Token vma_tok = p->current;
    if (!check(p, TOKEN_VMA) && !check(p, TOKEN_IDENTIFIER)) {
        error_at(p, p->current, "expected a VMA or declared name as the STORE target");
    }
    advance(p);
    ASTNode *n = ast_new(NODE_STORE_STMT, line);
    n->as.store_stmt.value = value;
    n->as.store_stmt.target_vma = token_text(vma_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_clean_stmt(Parser *p) {
    int line = p->current.line;
    advance(p);
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

/* Statement-form builtin command: NAME operand[, operand...]  (no parens) */
static ASTNode *parse_command_stmt(Parser *p, CommandKind kind) {
    int line = p->current.line;
    advance(p); /* command keyword */
    ASTNode *n = ast_new(NODE_COMMAND, line);
    n->as.command.kind = kind;
    nodelist_init(&n->as.command.args);
    if (!check(p, TOKEN_NEWLINE) && !check(p, TOKEN_EOF)) {
        do {
            nodelist_push(&n->as.command.args, parse_expression(p));
        } while (match(p, TOKEN_COMMA));
    }
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_cleanall_stmt(Parser *p) {
    int line = p->current.line;
    advance(p);
    ASTNode *n = ast_new(NODE_CLEANALL_STMT, line);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_autoclean_stmt(Parser *p) {
    int line = p->current.line;
    advance(p);
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
    advance(p);
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

static ASTNode *parse_job_decl(Parser *p) {
    int line = p->current.line;
    advance(p); /* JOB */

    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected a job name after JOB");

    int param_count = 0;
    JobParam *params = parse_job_params(p, &param_count);

    expect_statement_end(p);

    const VOTokenType terms[] = { TOKEN_ENDJOB };
    ASTNode *body = parse_block_until(p, terms, 1);
    expect(p, TOKEN_ENDJOB, "expected ENDJOB to close job");
    expect_statement_end(p);

    ASTNode *n = ast_new(NODE_JOB_DECL, line);
    n->as.job_decl.name = token_text(name_tok);
    n->as.job_decl.params = params;
    n->as.job_decl.param_count = param_count;
    n->as.job_decl.body = body;
    return n;
}

static ASTNode *parse_give_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* GIVE */
    ASTNode *value = parse_expression(p);
    ASTNode *n = ast_new(NODE_GIVE_STMT, line);
    n->as.give_stmt.value = value;
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_demand_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* DEMAND */
    ASTNode *condition = parse_expression(p);
    char *message = NULL;
    if (check(p, TOKEN_TEX_LITERAL)) {
        /* Grab the message string */
        char *raw = token_text(p->current);
        int len = (int)strlen(raw);
        message = malloc(len - 1);
        memcpy(message, raw + 1, len - 2);
        message[len - 2] = '\0';
        free(raw);
        advance(p);
    }
    ASTNode *n = ast_new(NODE_DEMAND_STMT, line);
    n->as.demand_stmt.condition = condition;
    n->as.demand_stmt.message = message;
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_serve_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* SERVE */
    ASTNode *value = parse_expression(p);
    ASTNode *n = ast_new(NODE_SERVE_STMT, line);
    n->as.serve_stmt.value = value;
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_do_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* DO */
    expect_statement_end(p);

    const VOTokenType try_terms[] = { TOKEN_GRABE };
    ASTNode *try_block = parse_block_until(p, try_terms, 1);

    expect(p, TOKEN_GRABE, "expected GRABE after DO block");
    expect_statement_end(p);

    const VOTokenType catch_terms[] = { TOKEN_ENDDO };
    ASTNode *catch_block = parse_block_until(p, catch_terms, 1);

    expect(p, TOKEN_ENDDO, "expected ENDDO to close DO/GRABE");
    expect_statement_end(p);

    ASTNode *n = ast_new(NODE_DO_STMT, line);
    n->as.do_stmt.try_block = try_block;
    n->as.do_stmt.catch_block = catch_block;
    return n;
}

static ASTNode *parse_bring_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* BRING */

    /* Collect everything from here to end of line as a raw path.
       The path may be tokenized as IDENTifiers, DOTs, SLASHes, etc. */
    char path_buf[1024];
    int pos = 0;

    while (!check(p, TOKEN_NEWLINE) && !check(p, TOKEN_EOF)) {
        /* Copy the token text */
        for (int i = 0; i < p->current.length && pos < (int)sizeof(path_buf) - 1; i++)
            path_buf[pos++] = p->current.start[i];
        advance(p);
    }
    path_buf[pos] = '\0';

    /* Trim trailing whitespace */
    while (pos > 0 && (path_buf[pos-1] == ' ' || path_buf[pos-1] == '\t'))
        path_buf[--pos] = '\0';

    ASTNode *n = ast_new(NODE_BRING_STMT, line);
    n->as.bring_stmt.path = strdup(path_buf);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_ship_stmt(Parser *p) {
    int line = p->current.line;
    advance(p); /* SHIP */
    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected a member name after SHIP");
    ASTNode *n = ast_new(NODE_SHIP_STMT, line);
    n->as.ship_stmt.name = token_text(name_tok);
    expect_statement_end(p);
    return n;
}

static ASTNode *parse_peice_decl(Parser *p) {
    int line = p->current.line;
    advance(p); /* PEICE */

    Token name_tok = p->current;
    expect(p, TOKEN_IDENTIFIER, "expected a piece name after PEICE");
    expect_statement_end(p);

    const VOTokenType terms[] = { TOKEN_ENDPEICE };
    ASTNode *body = parse_block_until(p, terms, 1);
    expect(p, TOKEN_ENDPEICE, "expected ENDPEICE");
    expect_statement_end(p);

    ASTNode *n = ast_new(NODE_PEICE_DECL, line);
    n->as.peice_decl.name = token_text(name_tok);
    n->as.peice_decl.body = body;
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
            ASTNode *raw = parse_expression(p);
            cond = ast_new(NODE_UNARY, bline);
            cond->as.unary.op = TOKEN_NOT;
            cond->as.unary.operand = raw;
        }
        expect_statement_end(p);
        VOTokenType only_endif[] = { TOKEN_ENDIF };
        ASTNode *block = parse_block_until(p, only_endif, 1);
        ASTNode *branch = ast_new(NODE_IF_BRANCH, bline);
        branch->as.if_branch.condition = cond;
        branch->as.if_branch.block = block;
        nodelist_push(&if_stmt->as.if_stmt.branches, branch);
    }

    expect(p, TOKEN_ENDIF, "expected ENDIF to close this IF chain");
    expect_statement_end(p);
    return if_stmt;
}

static ASTNode *parse_while_stmt(Parser *p) {
    int line = p->current.line;
    advance(p);
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
    advance(p);
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
    advance(p);

    ASTNode *n = ast_new(NODE_WHEN_STMT, line);

    if (check(p, TOKEN_PROGRAM)) {
        advance(p);
        expect(p, TOKEN_START, "expected START after WHEN PROGRAM");
        n->as.when_stmt.kind = WHEN_PROGRAM_START;
    } else if ((check(p, TOKEN_VMA) || check(p, TOKEN_IDENTIFIER)) && p->next.type == TOKEN_CHANGED) {
        Token target_tok = p->current;
        advance(p);
        advance(p);
        n->as.when_stmt.kind = WHEN_VMA_CHANGED;
        n->as.when_stmt.vma_name = token_text(target_tok);
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
        case TOKEN_VAR:       return parse_var_decl(p);
        case TOKEN_HARD:      return parse_hard_decl(p);
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
        case TOKEN_JOB:       return parse_job_decl(p);
        case TOKEN_GIVE:      return parse_give_stmt(p);
        case TOKEN_DEMAND:    return parse_demand_stmt(p);
        case TOKEN_SERVE:     return parse_serve_stmt(p);
        case TOKEN_DO:        return parse_do_stmt(p);
        case TOKEN_BRING:     return parse_bring_stmt(p);
        case TOKEN_SHIP:      return parse_ship_stmt(p);
        case TOKEN_PEICE:     return parse_peice_decl(p);

        /* v1.4 statement-form commands */
        case TOKEN_ATTACH:    return parse_command_stmt(p, CMD_ATTACH);
        case TOKEN_PLACE:     return parse_command_stmt(p, CMD_PLACE);
        case TOKEN_ERASE:     return parse_command_stmt(p, CMD_ERASE);
        case TOKEN_SEAL:      return parse_command_stmt(p, CMD_SEAL);
        case TOKEN_PUT:       return parse_command_stmt(p, CMD_PUT);
        case TOKEN_MOVE:      return parse_command_stmt(p, CMD_MOVE);
        case TOKEN_MAKE:      return parse_command_stmt(p, CMD_MAKE);
        case TOKEN_RECALL:    return parse_command_stmt(p, CMD_RECALL);
        case TOKEN_CLONE:     return parse_command_stmt(p, CMD_CLONE);
        case TOKEN_DELIVER:   return parse_command_stmt(p, CMD_DELIVER);
        case TOKEN_HOLD:      return parse_command_stmt(p, CMD_HOLD);
        case TOKEN_HALT:      return parse_command_stmt(p, CMD_HALT);
        case TOKEN_SEIZE:     return parse_command_stmt(p, CMD_SEIZE);
        case TOKEN_RELEASE:   return parse_command_stmt(p, CMD_RELEASE);
        case TOKEN_ALIGN:     return parse_command_stmt(p, CMD_ALIGN);
        case TOKEN_ARM:       return parse_command_stmt(p, CMD_ARM);
        case TOKEN_DISARM:    return parse_command_stmt(p, CMD_DISARM);
        case TOKEN_FIRE:      return parse_command_stmt(p, CMD_FIRE);
        case TOKEN_RANK:      return parse_command_stmt(p, CMD_RANK);
        case TOKEN_KILL:      return parse_command_stmt(p, CMD_KILL);
        case TOKEN_SCREEN:    return parse_command_stmt(p, CMD_SCREEN);
        case TOKEN_LINK:      return parse_command_stmt(p, CMD_LINK);
        case TOKEN_DRAW:      return parse_command_stmt(p, CMD_DRAW);

        case TOKEN_IFNOT:
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
