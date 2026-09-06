CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

all: vo_lex vo_parse vo_analyze vo_run

vo_lex: main.c lexer.c token.h lexer.h
	$(CC) $(CFLAGS) -o vo_lex main.c lexer.c

vo_parse: main_parser.c parser.c ast.c lexer.c token.h lexer.h parser.h ast.h
	$(CC) $(CFLAGS) -o vo_parse main_parser.c parser.c ast.c lexer.c

vo_analyze: main_analyze.c parser.c ast.c analyzer.c lexer.c token.h lexer.h parser.h ast.h analyzer.h
	$(CC) $(CFLAGS) -o vo_analyze main_analyze.c parser.c ast.c analyzer.c lexer.c

vo_run: main_run.c parser.c ast.c analyzer.c runtime.c lexer.c token.h lexer.h parser.h ast.h analyzer.h runtime.h
	$(CC) $(CFLAGS) -o vo_run main_run.c parser.c ast.c analyzer.c runtime.c lexer.c -lm

clean:
	rm -f vo_lex vo_parse vo_analyze vo_run
