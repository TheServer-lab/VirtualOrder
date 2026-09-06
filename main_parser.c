#include <stdio.h>
#include <stdlib.h>
#include "parser.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (fread(buf, 1, size, f) != (size_t)size) { perror("fread"); exit(1); }
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
    Parser parser;
    parser_init(&parser, source);

    ASTNode *program = parser_parse_program(&parser);

    if (parser.had_error) {
        fprintf(stderr, "\nParsing finished with errors.\n");
    }

    ast_print(program, 0);

    free(source);
    return parser.had_error ? 1 : 0;
}
