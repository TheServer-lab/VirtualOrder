#ifndef VO_PARSER_H
#define VO_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token next;      /* one token of extra lookahead, needed to disambiguate
                         `WHEN A1 CHANGED` from `WHEN A1 < 100`, and
                         `IDENTIFIER:` labels from assignments            */
    int had_error;
} Parser;

void    parser_init(Parser *p, const char *source);
ASTNode *parser_parse_program(Parser *p);

#endif
