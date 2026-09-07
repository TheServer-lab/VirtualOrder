CC = gcc

# codegen.c/vo_rt.c use strdup() and open_memstream(). strdup() is POSIX,
# not plain C11, so strict -std=c11 alone doesn't declare it under glibc -
# hence -D_POSIX_C_SOURCE=200809L (keeps -std=c11 as-is otherwise; MinGW's
# runtime provides strdup() regardless of this macro, so it's harmless
# there too). open_memstream() itself doesn't exist on MinGW at all;
# codegen.c gets a portable replacement via memstream_compat.h instead.
CFLAGS = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -Wno-format-truncation

# `make` sets $(OS) to "Windows_NT" automatically when run from a native
# Windows toolchain (including MSYS2's MINGW64 shell) - use that to figure
# out which native binary format plain $(CC) actually produces, since
# that's what voc's ABI needs to match (see the note in main_compile.c's
# main()). The *other* platform becomes an optional cross build.
ifeq ($(OS),Windows_NT)
    HOST_TARGET  := windows
    CROSS_TARGET := linux
    CROSS_CC     := x86_64-linux-gnu-gcc
else
    HOST_TARGET  := linux
    CROSS_TARGET := windows
    CROSS_CC     := x86_64-w64-mingw32-gcc
endif

HAVE_CROSS := $(shell command -v $(CROSS_CC) >/dev/null 2>&1 && echo yes)

.PHONY: all runtime cross-runtime test clean

all: vo_lex vo_parse vo_analyze vo_run voc runtime cross-runtime

vo_lex: main.c lexer.c token.h lexer.h
	$(CC) $(CFLAGS) -o vo_lex main.c lexer.c

vo_parse: main_parser.c parser.c ast.c lexer.c token.h lexer.h parser.h ast.h
	$(CC) $(CFLAGS) -o vo_parse main_parser.c parser.c ast.c lexer.c

vo_analyze: main_analyze.c parser.c ast.c analyzer.c lexer.c token.h lexer.h parser.h ast.h analyzer.h
	$(CC) $(CFLAGS) -o vo_analyze main_analyze.c parser.c ast.c analyzer.c lexer.c

vo_run: main_run.c parser.c ast.c analyzer.c runtime.c lexer.c token.h lexer.h parser.h ast.h analyzer.h runtime.h
	$(CC) $(CFLAGS) -o vo_run main_run.c parser.c ast.c analyzer.c runtime.c lexer.c -lm

# ---- native compiler driver: source.vo -> .s or a linked binary ----
voc: main_compile.c parser.c ast.c analyzer.c codegen.c lexer.c token.h lexer.h parser.h ast.h analyzer.h codegen.h vo_rt.h memstream_compat.h
	$(CC) $(CFLAGS) -o voc main_compile.c parser.c ast.c analyzer.c codegen.c lexer.c

# ---- runtime support objects voc links every compiled program
#      against, for whichever target this host's own $(CC) natively
#      produces. voc looks for these next to its own binary as
#      vo_rt_<target>.o / vo_entry_<target>.o (flat layout, no build/
#      dir) - see find_runtime_objects() in main_compile.c. ----
runtime: vo_rt_$(HOST_TARGET).o vo_entry_$(HOST_TARGET).o

vo_rt_$(HOST_TARGET).o: vo_rt.c vo_rt.h
	$(CC) $(CFLAGS) -c -o $@ vo_rt.c

vo_entry_$(HOST_TARGET).o: vo_entry.c vo_rt.h
	$(CC) $(CFLAGS) -c -o $@ vo_entry.c

# ---- the *other* target, only if a cross toolchain for it is present ----
cross-runtime:
ifeq ($(HAVE_CROSS),yes)
	$(MAKE) vo_rt_$(CROSS_TARGET).o vo_entry_$(CROSS_TARGET).o
else
	@echo "note: $(CROSS_CC) not found - skipping $(CROSS_TARGET) runtime objects."
	@echo "      'voc --target=$(CROSS_TARGET)' won't work until they're built, e.g.:"
	@echo "       make cross-runtime CROSS_CC=<your cross compiler>"
endif

vo_rt_$(CROSS_TARGET).o: vo_rt.c vo_rt.h
	$(CROSS_CC) $(CFLAGS) -c -o $@ vo_rt.c

vo_entry_$(CROSS_TARGET).o: vo_entry.c vo_rt.h
	$(CROSS_CC) $(CFLAGS) -c -o $@ vo_entry.c

# ---- quick smoke test: same program through the interpreter and
#      through voc's native output, output must match ----
test: all
	@echo 'VAR NUM Balance EAQ 1000' > voc_smoke.vo
	@echo 'Balance -= 200' >> voc_smoke.vo
	@echo 'SHOW "Balance: $$" + Balance' >> voc_smoke.vo
	@./vo_run voc_smoke.vo > voc_smoke.interp.out
	@./voc voc_smoke.vo -o voc_smoke.bin >/dev/null
	@./voc_smoke.bin > voc_smoke.native.out
	@diff -q voc_smoke.interp.out voc_smoke.native.out \
		&& echo "OK: interpreter and native output match" \
		|| echo "MISMATCH: see voc_smoke.*.out"
	@rm -f voc_smoke.vo voc_smoke.bin voc_smoke.exe voc_smoke.interp.out voc_smoke.native.out

clean:
	rm -f vo_lex vo_parse vo_analyze vo_run voc *.o *.exe
