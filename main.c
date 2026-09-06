#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.vo>\n", argv[0]);
        return 1;
    }

    char *source = read_file(argv[1]);
    Lexer lx;
    lexer_init(&lx, source);

    int errors = 0;
    for (;;) {
        Token t = lexer_next_token(&lx);

        if (t.type == TOKEN_NEWLINE) {
            printf("%4d:%-3d  NEWLINE\n", t.line, t.col);
            continue;
        }
        if (t.type == TOKEN_ERROR) {
            printf("%4d:%-3d  ERROR    %.*s\n", t.line, t.col, t.length, t.start);
            errors++;
            continue;
        }

        printf("%4d:%-3d  %-14s %.*s\n",
               t.line, t.col, token_type_name(t.type), t.length, t.start);

        if (t.type == TOKEN_EOF) break;
    }

    free(source);

    if (errors) {
        fprintf(stderr, "\n%d lexer error(s)\n", errors);
        return 1;
    }
    return 0;
}
