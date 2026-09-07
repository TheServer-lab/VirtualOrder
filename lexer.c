#include <string.h>
#include <ctype.h>
#include "lexer.h"

/* ---------------------------------------------------------------------
 * Keyword table
 * Virtual Order keywords are case-sensitive and always written in UPPERCASE.
 * Anything alnum/underscore that is not in this table, and does not
 * match the VMA pattern below, is a plain IDENTIFIER.
 * ------------------------------------------------------------------- */
typedef struct { const char *text; VOTokenType type; } Keyword;

static const Keyword KEYWORDS[] = {
    {"VAR",        TOKEN_VAR},
    {"HARD",       TOKEN_HARD},
    {"EAQ",        TOKEN_EAQ},
    {"STORE",      TOKEN_STORE},
    {"LOAD",       TOKEN_LOAD},
    {"SHOW",       TOKEN_SHOW},
    {"CLEANALL",   TOKEN_CLEANALL},
    {"CLEAN",      TOKEN_CLEAN},
    {"AUTOCLEAN",  TOKEN_AUTOCLEAN},
    {"ON",         TOKEN_ON},
    {"OFF",        TOKEN_OFF},

    {"ENDIF",      TOKEN_ENDIF},
    {"IFNOT",      TOKEN_IFNOT},
    {"IF",         TOKEN_IF},
    {"ORIF",       TOKEN_ORIF},
    {"ENDWHILE",   TOKEN_ENDWHILE},
    {"WHILE",      TOKEN_WHILE},
    {"ENDFOR",     TOKEN_ENDFOR},
    {"FOR",        TOKEN_FOR},
    {"TO",         TOKEN_TO},
    {"GOTO",       TOKEN_GOTO},

    {"ENDWHEN",    TOKEN_ENDWHEN},
    {"WHEN",       TOKEN_WHEN},
    {"CHANGED",    TOKEN_CHANGED},
    {"PROGRAM",    TOKEN_PROGRAM},
    {"START",      TOKEN_START},
    {"OLD_VALUE",  TOKEN_OLD_VALUE},
    {"NEW_VALUE",  TOKEN_NEW_VALUE},

    {"NOT",        TOKEN_NOT},
    {"AND",        TOKEN_AND},
    {"OR",         TOKEN_OR},
    {"XOR",        TOKEN_XOR},

    {"NUM",        TOKEN_TYPE_NUM},
    {"DEC",        TOKEN_TYPE_DEC},
    {"TEX",        TOKEN_TYPE_TEX},
    {"YN",         TOKEN_TYPE_YN},
    {"COLL",       TOKEN_TYPE_COLL},
    {"EMP",        TOKEN_TYPE_EMP},

    {"YES",        TOKEN_YES},
    {"NO",         TOKEN_NO},

    {"LENGTH",     TOKEN_LENGTH},

    {"JOB",        TOKEN_JOB},
    {"ENDJOB",     TOKEN_ENDJOB},
    {"GIVE",       TOKEN_GIVE},

    {"PEICE",      TOKEN_PEICE},
    {"ENDPEICE",   TOKEN_ENDPEICE},
    {"BRING",      TOKEN_BRING},
    {"SHIP",       TOKEN_SHIP},

    {"DEMAND",     TOKEN_DEMAND},
    {"DO",         TOKEN_DO},
    {"GRABE",      TOKEN_GRABE},
    {"ENDDO",      TOKEN_ENDDO},
    {"SERVE",      TOKEN_SERVE},
    {"ISSUE",      TOKEN_ISSUE},
};
#define NUM_KEYWORDS (int)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

const char *token_type_name(VOTokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        case TOKEN_NUM_LITERAL: return "NUM_LITERAL";
        case TOKEN_DEC_LITERAL: return "DEC_LITERAL";
        case TOKEN_TEX_LITERAL: return "TEX_LITERAL";
        case TOKEN_VMA: return "VMA";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_TYPE_NUM: return "TYPE_NUM";
        case TOKEN_TYPE_DEC: return "TYPE_DEC";
        case TOKEN_TYPE_TEX: return "TYPE_TEX";
        case TOKEN_TYPE_YN: return "TYPE_YN";
        case TOKEN_TYPE_COLL: return "TYPE_COLL";
        case TOKEN_TYPE_EMP: return "TYPE_EMP";
        case TOKEN_VAR: return "VAR";
        case TOKEN_HARD: return "HARD";
        case TOKEN_EAQ: return "EAQ";
        case TOKEN_STORE: return "STORE";
        case TOKEN_LOAD: return "LOAD";
        case TOKEN_SHOW: return "SHOW";
        case TOKEN_CLEAN: return "CLEAN";
        case TOKEN_CLEANALL: return "CLEANALL";
        case TOKEN_AUTOCLEAN: return "AUTOCLEAN";
        case TOKEN_ON: return "ON";
        case TOKEN_OFF: return "OFF";
        case TOKEN_IF: return "IF";
        case TOKEN_ORIF: return "ORIF";
        case TOKEN_IFNOT: return "IFNOT";
        case TOKEN_ENDIF: return "ENDIF";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_ENDWHILE: return "ENDWHILE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_TO: return "TO";
        case TOKEN_ENDFOR: return "ENDFOR";
        case TOKEN_GOTO: return "GOTO";
        case TOKEN_WHEN: return "WHEN";
        case TOKEN_ENDWHEN: return "ENDWHEN";
        case TOKEN_CHANGED: return "CHANGED";
        case TOKEN_PROGRAM: return "PROGRAM";
        case TOKEN_START: return "START";
        case TOKEN_OLD_VALUE: return "OLD_VALUE";
        case TOKEN_NEW_VALUE: return "NEW_VALUE";
        case TOKEN_NOT: return "NOT";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_XOR: return "XOR";
        case TOKEN_YES: return "YES";
        case TOKEN_NO: return "NO";
        case TOKEN_EMP: return "EMP";
        case TOKEN_LENGTH: return "LENGTH";
        case TOKEN_JOB: return "JOB";
        case TOKEN_ENDJOB: return "ENDJOB";
        case TOKEN_GIVE: return "GIVE";
        case TOKEN_PEICE: return "PEICE";
        case TOKEN_ENDPEICE: return "ENDPEICE";
        case TOKEN_BRING: return "BRING";
        case TOKEN_SHIP: return "SHIP";
        case TOKEN_DEMAND: return "DEMAND";
        case TOKEN_DO: return "DO";
        case TOKEN_GRABE: return "GRABE";
        case TOKEN_ENDDO: return "ENDDO";
        case TOKEN_SERVE: return "SERVE";
        case TOKEN_ISSUE: return "ISSUE";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_PERCENT: return "PERCENT";
        case TOKEN_POWER: return "POWER";
        case TOKEN_SHL: return "SHL";
        case TOKEN_SHR: return "SHR";
        case TOKEN_AMP: return "AMP";
        case TOKEN_CARET: return "CARET";
        case TOKEN_PIPE: return "PIPE";
        case TOKEN_EQEQ: return "EQEQ";
        case TOKEN_NEQ: return "NEQ";
        case TOKEN_LT: return "LT";
        case TOKEN_GT: return "GT";
        case TOKEN_LE: return "LE";
        case TOKEN_GE: return "GE";
        case TOKEN_ASSIGN: return "ASSIGN";
        case TOKEN_PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TOKEN_MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TOKEN_STAR_ASSIGN: return "STAR_ASSIGN";
        case TOKEN_SLASH_ASSIGN: return "SLASH_ASSIGN";
        case TOKEN_PERCENT_ASSIGN: return "PERCENT_ASSIGN";
        case TOKEN_INCREMENT: return "INCREMENT";
        case TOKEN_DECREMENT: return "DECREMENT";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_COLON: return "COLON";
        case TOKEN_DOT: return "DOT";
        case TOKEN_NEWLINE: return "NEWLINE";
        default: return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------
 * Low level cursor helpers
 * ------------------------------------------------------------------- */
static int is_at_end(Lexer *lx) { return *lx->current == '\0'; }

static char advance(Lexer *lx) {
    char c = *lx->current++;
    lx->col++;
    if (c == '\n') { lx->line++; lx->col = 1; }
    return c;
}

static char peek(Lexer *lx) { return *lx->current; }
static char peek_next(Lexer *lx) {
    if (is_at_end(lx)) return '\0';
    return lx->current[1];
}

static int match(Lexer *lx, char expected) {
    if (is_at_end(lx)) return 0;
    if (*lx->current != expected) return 0;
    lx->current++;
    lx->col++;
    return 1;
}

static Token make_token(Lexer *lx, VOTokenType type) {
    Token t;
    t.type   = type;
    t.start  = lx->start;
    t.length = (int)(lx->current - lx->start);
    t.line   = lx->line;
    t.col    = lx->col - t.length;
    return t;
}

static Token error_token(Lexer *lx, const char *msg) {
    Token t;
    t.type   = TOKEN_ERROR;
    t.start  = msg;
    t.length = (int)strlen(msg);
    t.line   = lx->line;
    t.col    = lx->col;
    return t;
}

/* ---------------------------------------------------------------------
 * Whitespace & comments
 *
 * ";"  starts a line comment, runs to end of line.
 * ";;" toggles a block comment: the first ";;" opens it, everything
 *      (including newlines) is skipped until the next ";;" closes it.
 * ------------------------------------------------------------------- */
static void skip_whitespace_and_comments(Lexer *lx) {
    for (;;) {
        if (lx->in_block_comment) {
            if (is_at_end(lx)) return;
            if (peek(lx) == ';' && peek_next(lx) == ';') {
                advance(lx); advance(lx);
                lx->in_block_comment = 0;
                continue;
            }
            advance(lx);
            continue;
        }

        char c = peek(lx);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lx);
                break;

            case '\n':
                return;

            case ';':
                if (peek_next(lx) == ';') {
                    advance(lx); advance(lx);
                    lx->in_block_comment = 1;
                    break;
                }
                while (peek(lx) != '\n' && !is_at_end(lx)) advance(lx);
                break;

            default:
                return;
        }
    }
}

/* ---------------------------------------------------------------------
 * VMA detection: [A-Z]+[0-9]+  (uppercase letters, then digits, no mix)
 * ------------------------------------------------------------------- */
static int is_vma_lexeme(const char *s, int len) {
    int i = 0;
    int letter_count = 0, digit_count = 0;

    while (i < len && s[i] >= 'A' && s[i] <= 'Z') { i++; letter_count++; }
    if (letter_count == 0) return 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digit_count++; }
    if (digit_count == 0) return 0;

    return i == len;
}

/* ---------------------------------------------------------------------
 * Identifiers, keywords, VMAs
 * ------------------------------------------------------------------- */
static Token scan_identifier_or_keyword_or_vma(Lexer *lx) {
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);

    int len = (int)(lx->current - lx->start);
    const char *lexeme = lx->start;

    for (int k = 0; k < NUM_KEYWORDS; k++) {
        size_t klen = strlen(KEYWORDS[k].text);
        if ((int)klen == len && strncmp(KEYWORDS[k].text, lexeme, len) == 0) {
            return make_token(lx, KEYWORDS[k].type);
        }
    }

    if (is_vma_lexeme(lexeme, len)) {
        return make_token(lx, TOKEN_VMA);
    }

    return make_token(lx, TOKEN_IDENTIFIER);
}

/* ---------------------------------------------------------------------
 * Numbers: NUM_LITERAL (123) or DEC_LITERAL (3.14)
 * ------------------------------------------------------------------- */
static Token scan_number(Lexer *lx) {
    while (isdigit((unsigned char)peek(lx))) advance(lx);

    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        advance(lx);
        while (isdigit((unsigned char)peek(lx))) advance(lx);
        return make_token(lx, TOKEN_DEC_LITERAL);
    }
    return make_token(lx, TOKEN_NUM_LITERAL);
}

/* ---------------------------------------------------------------------
 * Strings: "..." with minimal escape support (\" \\ \n \t)
 * ------------------------------------------------------------------- */
static Token scan_string(Lexer *lx) {
    while (peek(lx) != '"' && !is_at_end(lx)) {
        if (peek(lx) == '\\' && peek_next(lx) != '\0') {
            advance(lx);
        }
        advance(lx);
    }
    if (is_at_end(lx)) return error_token(lx, "Unterminated string literal");
    advance(lx);
    return make_token(lx, TOKEN_TEX_LITERAL);
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */
void lexer_init(Lexer *lx, const char *source) {
    lx->source  = source;
    lx->start   = source;
    lx->current = source;
    lx->line    = 1;
    lx->col     = 1;
    lx->in_block_comment = 0;
}

Token lexer_next_token(Lexer *lx) {
    skip_whitespace_and_comments(lx);
    lx->start = lx->current;

    if (is_at_end(lx)) return make_token(lx, TOKEN_EOF);

    char c = advance(lx);

    if (c == '\n') return make_token(lx, TOKEN_NEWLINE);

    if (isalpha((unsigned char)c) || c == '_')
        return scan_identifier_or_keyword_or_vma(lx);

    if (isdigit((unsigned char)c))
        return scan_number(lx);

    if (c == '"')
        return scan_string(lx);

    switch (c) {
        case '(': return make_token(lx, TOKEN_LPAREN);
        case ')': return make_token(lx, TOKEN_RPAREN);
        case '[': return make_token(lx, TOKEN_LBRACKET);
        case ']': return make_token(lx, TOKEN_RBRACKET);
        case ',': return make_token(lx, TOKEN_COMMA);
        case ':': return make_token(lx, TOKEN_COLON);
        case '.': return make_token(lx, TOKEN_DOT);

        case '*':
            if (match(lx, '*')) return make_token(lx, TOKEN_POWER);
            if (match(lx, '=')) return make_token(lx, TOKEN_STAR_ASSIGN);
            return make_token(lx, TOKEN_STAR);

        case '/':
            if (match(lx, '=')) return make_token(lx, TOKEN_SLASH_ASSIGN);
            return make_token(lx, TOKEN_SLASH);

        case '%':
            if (match(lx, '=')) return make_token(lx, TOKEN_PERCENT_ASSIGN);
            return make_token(lx, TOKEN_PERCENT);

        case '+':
            if (match(lx, '+')) return make_token(lx, TOKEN_INCREMENT);
            if (match(lx, '=')) return make_token(lx, TOKEN_PLUS_ASSIGN);
            return make_token(lx, TOKEN_PLUS);

        case '-':
            if (match(lx, '-')) return make_token(lx, TOKEN_DECREMENT);
            if (match(lx, '=')) return make_token(lx, TOKEN_MINUS_ASSIGN);
            return make_token(lx, TOKEN_MINUS);

        case '<':
            if (match(lx, '<')) return make_token(lx, TOKEN_SHL);
            if (match(lx, '=')) return make_token(lx, TOKEN_LE);
            return make_token(lx, TOKEN_LT);

        case '>':
            if (match(lx, '>')) return make_token(lx, TOKEN_SHR);
            if (match(lx, '=')) return make_token(lx, TOKEN_GE);
            return make_token(lx, TOKEN_GT);

        case '&': return make_token(lx, TOKEN_AMP);
        case '^': return make_token(lx, TOKEN_CARET);
        case '|': return make_token(lx, TOKEN_PIPE);

        case '=':
            if (match(lx, '=')) return make_token(lx, TOKEN_EQEQ);
            return make_token(lx, TOKEN_ASSIGN);

        case '!':
            if (match(lx, '=')) return make_token(lx, TOKEN_NEQ);
            return error_token(lx, "Unexpected character '!'");

        default:
            return error_token(lx, "Unexpected character");
    }
}
