# Packaging `.vo` programs into executables

This is the "PyInstaller for Virtual Order" workflow: turn a `.vo` source
file into a standalone native binary (or inspect the generated assembly).

## One-time setup

```
make            # builds vo_lex/vo_parse/vo_analyze/vo_run, voc, and voc's
                # runtime objects, all under build/
make install    # optional: stage the same layout into /usr/local
                # (or `make install PREFIX=~/.local`)
```

`make` also tries to build a Windows runtime (needed for `--target=windows`)
if it finds a `x86_64-w64-mingw32-gcc` cross toolchain on your PATH. If it
doesn't find one, everything else still builds fine — Windows output just
won't be available until you build (or copy over) `build/lib/vo/windows/`
from a machine that has that toolchain:

```
make windows-runtime MINGW_CC=x86_64-w64-mingw32-gcc
```

## Producing a binary or assembly file

```
build/bin/voc program.vo                        # -> a.out (or a.exe on --target=windows)
build/bin/voc program.vo -o myprogram            # custom output path
build/bin/voc program.vo -S -o program.s         # emit x86-64 GAS assembly instead
build/bin/voc program.vo --target=windows -o app.exe
```

`voc` runs your program through the same lexer/parser/analyzer as `vo_run`,
so any syntax or semantic error is caught before anything is compiled to
machine code. It then lowers the AST straight to x86-64 assembly
(`codegen.c`) and links it against `vo_rt.o` (the small runtime support
library — arithmetic, printing, collections) and `vo_entry.c` (the
`main()` stub) using your system's `gcc` (or a mingw-w64 `gcc` for
Windows). No bytecode interpreter or AST ships inside the final binary —
it's a real, independent executable.

## Where `voc` finds its runtime objects

`voc` locates `vo_rt.o`/`vo_entry.o` relative to its own binary path (like
a normal compiler finds `crt0.o`), in this order:

1. `<install-prefix>/lib/vo/<target>/{vo_rt,vo_entry}.o` — the `make
   install` layout.
2. `<voc's own dir>/vo_rt_<target>.o` — a flat `build/`-style layout.
3. `vo_rt.c`/`vo_entry.c` found next to `voc` or in the current
   directory — compiled fresh as a last resort (slower, but useful
   straight out of a source checkout with nothing installed yet).

If none of these are found, `voc` fails with a clear error instead of
silently trying (and failing) some hardcoded path.

## Known limitation

`codegen.c`'s v1 backend doesn't yet lower `WHEN`/`ENDWHEN` event
handlers or `OLD_VALUE`/`NEW_VALUE` — `voc` reports this as a compile
error rather than silently mis-compiling. Programs using those constructs
should run under the tree-walking interpreter (`vo_run`) instead.
