<p align="center">
  <img src="assets/logo-wordmark.svg" alt="Virtual Order" width="600">
</p>

<p align="center">
  A VMA-oriented, assembly-inspired language with typed variables, structured control flow,
  and an edge-triggered event system.
</p>

---

## What is this

Virtual Order (`.vo`) is a small interpreted language built around **VMAs** — Virtual
Memory Areas addressed like spreadsheet cells (`A1`, `B7`, `AA12`) — plus the usual things
a language needs on top: typed declarations, `IF`/`WHILE`/`FOR`, collections, and a
`WHEN` block system for reacting to state changes.

```vo
VAR NUM Balance EAQ 1000
VAR COLL History EAQ []

AUTOCLEAN ON

WHEN A1 CHANGED
    History += NEW_VALUE
    SHOW "Balance: $" + A1
ENDWHEN

WHEN A1 < 100
    SHOW "Low balance warning"
ENDWHEN

A1 -= 950
```

Running that prints:

```
Balance: $50
Low balance warning
```

The toolchain is a conventional four-stage pipeline, each stage usable on its own:

```
source.vo  ->  lexer  ->  parser  ->  analyzer  ->  runtime
              (vo_lex)  (vo_parse) (vo_analyze)   (vo_run)
```

## Language highlights

- **Typed declarations** — `NUM`, `DEC`, `TEX`, `YN`, `COLL`, each `VAR` or `CONST`.
- **VMA memory model** — every declared identifier is backed by a real address
  (`A1`, `A2`, ... `Z9999`, `AA1`, ...), allocated first-fit and reusable once freed.
- **Structured control flow** — `IF`/`ORIF`/`IFNOT`/`ENDIF`, `WHILE`/`ENDWHILE`,
  `FOR ... TO ... ENDFOR`.
- **Edge-triggered events** — `WHEN <vma> CHANGED` fires only when a value actually
  changes; `WHEN <condition>` fires only on the false→true transition, never on every
  tick the condition happens to hold.
- **A real event queue** — triggered handlers are queued, not run inline: the
  currently executing statement or handler always finishes first, and a handler's own
  writes enqueue further events at the back of the same queue rather than recursing.
- **AUTOCLEAN scoping** — `WHEN` blocks and the top-level program are the only scopes;
  turn `AUTOCLEAN ON` and every declaration made in that scope is freed automatically
  when the scope exits.
- **Assembly-style GOTO** — `Label:` / `GOTO Label`, whole-program flat namespace,
  forward references allowed.

See [`VirtualOrder-v1.3—Language-Specification.md`](VirtualOrder-v1.3—Language-Specification.md) for the full language
specification.

## Building

Requires a C11 compiler (`gcc`/`clang`) and `make`. No other dependencies.

```
make        # builds vo_lex, vo_parse, vo_analyze, vo_run
```

| Binary        | Stage              | What it does                                            |
|---------------|--------------------|----------------------------------------------------------|
| `vo_lex`      | Lexer              | Prints the token stream for a `.vo` file.                |
| `vo_parse`    | Parser             | Prints the parsed AST.                                    |
| `vo_analyze`  | Semantic analysis  | Scope/type/VMA checks; reports errors and warnings.        |
| `vo_run`      | Full pipeline      | Lexes, parses, analyzes, then executes the program.        |

```
./vo_run test.vo
```

`make clean` removes the built binaries.

## Project layout

```
token.h, lexer.h/.c      tokenizer
ast.h/.c                 AST node types + debug printer
parser.h/.c              recursive-descent parser
analyzer.h/.c            scope/type/VMA static checks
runtime.h/.c             interpreter: VMA storage, event queue, GOTO
main*.c                  one entry point per pipeline stage
test.vo, stress_test.vo  example / stress-test programs
```

## Status

Actively evolving — expect the spec and runtime to move together as the language
firms up.

## License

Proprietary. All rights reserved — see [`LICENSE`](LICENSE). This is not open-source
software; no permission is granted to use, copy, modify, or distribute it without a
separate written agreement.
