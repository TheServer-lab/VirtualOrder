#ifndef VO_TOKEN_H
#define VO_TOKEN_H

typedef enum {
    /* End / error */
    TOKEN_EOF = 0,
    TOKEN_ERROR,

    /* Literals & identifiers */
    TOKEN_NUM_LITERAL,      /* 45, 1000            */
    TOKEN_DEC_LITERAL,      /* 3.14                */
    TOKEN_TEX_LITERAL,      /* "hello"             */
    TOKEN_VMA,              /* A1, AA3, ZZZZAAER1000 -> [A-Z]+[0-9]+ */
    TOKEN_IDENTIFIER,       /* Age, Balance, isActive */

    /* Declaration / memory keywords */
    TOKEN_VAR,
    TOKEN_CONST,
    TOKEN_EAQ,
    TOKEN_STORE,
    TOKEN_LOAD,
    TOKEN_SHOW,
    TOKEN_CLEAN,
    TOKEN_CLEANALL,
    TOKEN_AUTOCLEAN,
    TOKEN_ON,
    TOKEN_OFF,

    /* Control flow keywords */
    TOKEN_IF,
    TOKEN_ORIF,
    TOKEN_IFNOT,
    TOKEN_ENDIF,
    TOKEN_WHILE,
    TOKEN_ENDWHILE,
    TOKEN_FOR,
    TOKEN_TO,
    TOKEN_ENDFOR,
    TOKEN_GOTO,

    /* Event keywords */
    TOKEN_WHEN,
    TOKEN_ENDWHEN,
    TOKEN_CHANGED,
    TOKEN_PROGRAM,
    TOKEN_START,
    TOKEN_OLD_VALUE,
    TOKEN_NEW_VALUE,

    /* Logical keywords */
    TOKEN_NOT,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_XOR,

    /* Type keywords */
    TOKEN_TYPE_NUM,
    TOKEN_TYPE_DEC,
    TOKEN_TYPE_TEX,
    TOKEN_TYPE_YN,
    TOKEN_TYPE_COLL,

    /* Boolean / null literals */
    TOKEN_YES,
    TOKEN_NO,
    TOKEN_NULL,

    /* Builtins */
    TOKEN_LENGTH,

    /* Operators */
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT, TOKEN_POWER,
    TOKEN_SHL, TOKEN_SHR,
    TOKEN_AMP, TOKEN_CARET, TOKEN_PIPE,
    TOKEN_EQEQ, TOKEN_NEQ, TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE,
    TOKEN_ASSIGN,
    TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN, TOKEN_STAR_ASSIGN,
    TOKEN_SLASH_ASSIGN, TOKEN_PERCENT_ASSIGN,
    TOKEN_INCREMENT, TOKEN_DECREMENT,

    /* Punctuation */
    TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_COMMA, TOKEN_COLON,
    TOKEN_NEWLINE

} VOTokenType;

typedef struct {
    VOTokenType type;
    const char *start;   /* pointer into source buffer, NOT null-terminated */
    int         length;
    int         line;
    int         col;
} Token;

const char *token_type_name(VOTokenType type);

#endif
