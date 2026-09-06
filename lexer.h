#ifndef VO_LEXER_H
#define VO_LEXER_H

#include "token.h"

typedef struct {
    const char *source;   /* start of buffer                */
    const char *start;    /* start of current token         */
    const char *current;  /* current scan position          */
    int line;
    int col;              /* column of `start`               */
    int in_block_comment; /* 1 while inside a ;; ... ;; block */
} Lexer;

void  lexer_init(Lexer *lx, const char *source);
Token lexer_next_token(Lexer *lx);

#endif
