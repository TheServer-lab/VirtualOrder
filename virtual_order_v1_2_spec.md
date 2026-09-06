# Virtual Order v1.2 — Language Specification

*Revision notes: v1.1 resolved six ambiguities found in v1.0 — allocation order, refcount semantics, AUTOCLEAN scope, simultaneous event triggers, STORE/LOAD argument order, and event re-entrancy. v1.2 closes a further gap: whether `WHEN` conditions are level-triggered or edge-triggered, and what exactly makes `CHANGED` fire. See §11 for the full changelog.*

---

## 1. Operator Precedence Table

| Level | Operators | Associativity | Description |
|---|---|---|---|
| 1 | `()` | — | Parentheses |
| 2 | `**` | Right | Exponentiation |
| 3 | `* / %` | Left | Multiplication, Division, Modulo |
| 4 | `+ -` | Left | Addition, Subtraction |
| 5 | `<< >>` | Left | Bitwise shifts |
| 6 | `&` | Left | Bitwise AND |
| 7 | `^` | Left | Bitwise XOR |
| 8 | `\|` | Left | Bitwise OR |
| 9 | `== != < > <= >=` | Left | Comparison |
| 10 | `NOT` | Right | Logical NOT |
| 11 | `AND` | Left | Logical AND |
| 12 | `XOR` | Left | Logical XOR |
| 13 | `OR` | Left | Logical OR |
| 14 | `= += -= *= /= %=` | Right | Assignment |

`=` is assignment (level 14); `==` is equality comparison (level 9).

---

## 2. Conditional Constructs

### A. Standard IF chain
```asm
IF condition1
    statements
ORIF condition2
    statements
IFNOT                  ; no condition — this is the ELSE branch
    statements
ENDIF
```

### B. IFNOT with a condition (shorthand for IF NOT)
```asm
IFNOT A1 > 0            ; equivalent to: IF NOT (A1 > 0)
    SHOW "A1 is zero or negative"
ENDIF
```
`IFNOT` with no condition = else branch. `IFNOT <condition>` = `IF NOT <condition>`. Both forms are legal; the parser disambiguates by presence of a trailing expression.

**Note:** `IF`, `WHILE`, and `FOR` bodies do **not** introduce a new variable scope (see §6.D).

---

## 3. Increment / Decrement Rules

Statement-only — not usable inside expressions.

```asm
; Allowed
Counter++
A1--

; Errors
Result = Counter++ * 5   ; ❌
IF A1++ > 0               ; ❌
```

---

## 4. Boolean Conversion Rules

| Context | Rule |
|---|---|
| `NUM → YN` | `0` = NO, nonzero = YES |
| `DEC → YN` | `0.0` = NO, nonzero = YES |
| `TEX → YN` | `""` = NO, non-empty = YES |
| `COLL → YN` | `[]` = NO, non-empty = YES |

---

## 5. Identifier ↔ VMA Equivalence

```asm
VAR NUM Temp EAQ 25   ; allocates Temp → A1

Temp = 30             ; identical effect to:
A1 = 30
```

- VMA binding is stable for the identifier's lifetime.
- `CLEAN A1` also invalidates `Temp`.
- **An identifier can never be rebound to a different VMA.**
- **Aliasing is banned in v1.1**: `VAR NUM X EAQ A1` is a compile error if `A1` is already allocated to another identifier. (A future `ALIAS` construct may relax this — not part of v1.1.)

---

## 6. VMA Formal Specification

### A. Addressing Scheme (resolved)

Allocation always uses the numeric form — bare letters (`A`, `B`, `AA`) are **not** allocated on their own. Column letters exhaust numerically before advancing to the next letter combination:

```
A1, A2, ..., A9999,
B1, B2, ..., Z9999,
AA1, AA2, ..., ZZ9999,
AAA1, AAA2, ...
```

Format grammar: `VMA ::= [A-Z]+[0-9]+`

### B. Allocation Strategy (resolved)

First-fit over the address space in the order defined in §6.A. Freed addresses are reused before advancing the allocation frontier.

```asm
VAR NUM X EAQ 10   ; X → A1  (first free)
VAR NUM Y EAQ 20   ; Y → A2  (second free)
CLEAN A2           ; A2 freed
VAR NUM Z EAQ 30   ; Z → A2  (reused)
```

### C. VMA Table Structure (resolved)

Refcounting is removed — since aliasing is banned, no VMA can have more than one owner, so a boolean `Allocated` flag fully captures state:

```
┌─────────┬──────────┬─────────────┬───────────┬────────────┐
│ VMA     │ Type     │ Value       │ Allocated │ Identifier │
├─────────┼──────────┼─────────────┼───────────┼────────────┤
│ A1      │ NUM      │ 45          │ true      │ Age        │
│ A2      │ TEX      │ "John"      │ true      │ Name       │
│ B1      │ NUM      │ 10          │ true      │ Counter    │
│ B2      │ COLL     │ [1,2,3]     │ true      │ Scores     │
│ B3      │ NULL     │ NULL        │ false     │ -          │
└─────────┴──────────┴─────────────┴───────────┴────────────┘
```

### D. Lifetime Rules (resolved)

```asm
; 1. Manual cleanup
CLEAN A1              ; sets Allocated = false, clears identifier binding
                       ; if A1 = Age, Age becomes invalid

; 2. Auto cleanup — scoped to WHEN blocks and top-level program
AUTOCLEAN ON
AUTOCLEAN OFF

; 3. Global cleanup
CLEANALL               ; frees all VMAs, resets everything

; 4. Access after cleanup
; runtime error: "VMA A1 is not allocated"
```

**Scope model:** a VMA declared with `AUTOCLEAN ON` in effect auto-cleans when its *enclosing scope* exits. The only scope-introducing constructs in v1.1 are:
- a `WHEN ... ENDWHEN` block
- the top-level program

`IF`, `WHILE`, and `FOR` bodies do **not** introduce scope — a variable declared inside one lives until its enclosing `WHEN` block (or the program) ends.

```asm
AUTOCLEAN ON
WHEN PROGRAM START
    VAR NUM Temp EAQ 5      ; Temp → A1
    IF Temp > 0
        VAR NUM Extra EAQ 1  ; Extra → A2, scoped to this WHEN block, not the IF
    ENDIF
    SHOW Extra               ; legal
ENDWHEN
; A1, A2 auto-clean here
```

### E. VMA Events

```asm
WHEN A1 CHANGED
    SHOW "A1 changed: " + OLD_VALUE + " → " + NEW_VALUE
ENDWHEN

WHEN A1 > 100
    SHOW "A1 exceeded threshold"
ENDWHEN

WHEN A1 = 0
    SHOW "A1 reset to zero"
ENDWHEN
```

`OLD_VALUE` / `NEW_VALUE` are implicit read-only bindings available inside a `CHANGED` handler, valid only within that handler's body.

**Trigger semantics — edge-triggered, not level-triggered (resolved in v1.2):**

There are two ways a `WHEN` could behave: fire on *every* assignment where the condition currently holds ("level-triggered"), or fire only on the *transition* into holding true ("edge-triggered"). Virtual Order uses **edge-triggered** semantics for both forms:

- **Boolean `WHEN <condition>`** (e.g. `WHEN A1 < 100`, `WHEN A1 >= 100`): each such handler carries hidden state `last_state`, a boolean initialized to `false` at the moment the `WHEN` block is registered (i.e. when its enclosing scope — a `WHEN PROGRAM START` block or the top-level program — is entered). Registration itself does **not** evaluate the condition against the VMA's current value.

  On every assignment to a VMA referenced in the condition:
  1. Evaluate the condition → `current_state`.
  2. `last_state == false` and `current_state == true` → **fire**, then set `last_state = true`.
  3. `last_state == true` and `current_state == false` → do not fire; set `last_state = false` (this re-arms the trigger).
  4. Otherwise (`last_state == current_state`) → do nothing.

  This makes `WHEN A1 < 100` a genuine threshold-*crossing* event: it fires once when balance drops below 100, stays silent as it continues falling (60, 30, 5, ...), and won't fire again unless the balance first rises back to ≥100 and later drops below 100 again.

- **`WHEN <VMA> CHANGED`** does not use `last_state` — it compares the incoming value to the value already stored in the VMA table for that address:
  ```asm
  A1 = 50
  A1 = 50   ; CHANGED does NOT fire — new_value equals old_value
  A1 = 51   ; CHANGED fires — new_value differs from old_value
  ```
  Rule: **`CHANGED` fires if and only if `new_value != old_value`.** The VMA's stored value updates regardless of whether `CHANGED` fires.

**Simultaneous triggers (resolved):** if an assignment causes more than one `WHEN` handler on the same VMA to cross into true at once, **all** matching handlers fire, in the order they were declared in source. ("Cross into true" now means the edge-triggered condition in the rule above, not merely "currently evaluates true.")

**Event re-entrancy (resolved):** event dispatch is **queued**, not inline/recursive.
- An assignment enqueues any newly-true `WHEN` conditions; it does not run them immediately.
- The currently executing statement or handler finishes completely before the queue is drained.
- The queue drains FIFO; if a queued handler triggers further events, those go to the **back** of the queue (breadth-first), never onto the call stack.
- A max queue depth per program tick (default: 1000) guards against infinite trigger loops. Exceeding it raises: `"Event queue overflow: possible infinite trigger loop"`.

Example trace for `Balance += Transaction` where `Balance` is `A1`, given `WHEN A1 CHANGED`, `WHEN A1 < 100`, and `WHEN A1 < 0` are all registered:
1. Assignment completes; `A1`'s stored value updates from `old_value` to `new_value`.
2. `CHANGED` fires if `new_value != old_value`; enqueued if so.
3. Each boolean condition (`< 100`, `< 0`) is checked against its own `last_state`; only conditions that *newly* cross into true are enqueued (see edge-triggering rule above) — conditions that were already true before this assignment do not re-fire.
4. The current statement/handler finishes running.
5. Queue drains: each handler runs to completion; any events it triggers are appended to the back of the queue.

So a balance path of `1000 → 800 → 300 → 50 → -50` fires `< 100` exactly once (on the `300 → 50` transition) and `< 0` exactly once (on the `50 → -50` transition) — not on every assignment where the condition happens to still hold.

---

## 7. STORE / LOAD Grammar (resolved)

```
STORE ::= "STORE" (value | identifier) VMA
LOAD  ::= "LOAD" VMA
```

```asm
STORE 20 A1        ; store literal 20 into A1
STORE Age A1       ; store the value currently held by Age into A1
LOAD A1            ; load value from A1
```

---

## 8. Language Architecture

```
Virtual Order Source Code
        │
        ▼
      Lexer        — tokenizes keywords, operators, VMAs
        │
        ▼
      Parser       — builds AST with precedence (§1)
        │
        ▼
      Analyzer     — type checking, symbol resolution, scope checking
        │
        ▼
      VMA Table    — tracks allocation, identifier bindings (§6.C)
        │
        ▼
      Event Queue  — WHEN bindings, FIFO dispatch (§6.E)
        │
        ▼
      Runtime      — executes instructions
        │
        ▼
      Output       — console, files, GUI
```

---

## 9. Comprehensive Example (Banking System, v1.1 semantics)

```asm
;;
Complete Demo: Banking System
;;

VAR NUM Balance EAQ 1000          ; Balance → A1
VAR NUM Transaction EAQ 0         ; Transaction → A2
VAR TEX Status EAQ "Active"       ; Status → B1
VAR COLL History EAQ []           ; History → B2
VAR YN IsActive EAQ YES           ; IsActive → C1

AUTOCLEAN ON

WHEN A1 CHANGED
    History += NEW_VALUE          ; enqueues History's own CHANGED handler, if any
    SHOW "Balance: $" + A1
ENDWHEN

WHEN A1 < 100
    Status = "Low Balance"
    SHOW "Low Balance - Please deposit"
ENDWHEN

WHEN A1 < 0
    Status = "Overdrawn!"
    IsActive = NO
    SHOW "ACCOUNT OVERDRAWN!"
ENDWHEN

WHEN PROGRAM START
    SHOW "=== Banking System ==="
    SHOW "Initial Balance: $" + Balance

    Transaction = -200
    Balance += Transaction        ; 800

    Transaction = -500
    Balance += Transaction        ; 300

    Transaction = -250
    Balance += Transaction        ; 50 — CHANGED fires (50 != 300);
                                    ; "< 100" crosses false->true, fires;
                                    ; "< 0" is still false, does not fire

    Transaction = -100
    Balance += Transaction        ; -50 — CHANGED fires (-50 != 50);
                                    ; "< 100" was already true, stays true,
                                    ;   does NOT re-fire (edge-triggered);
                                    ; "< 0" crosses false->true, fires

    SHOW "Transaction History:"
    FOR I = 0 TO LENGTH(History) - 1
        SHOW "  $" + History[I]
    ENDFOR

    IF Status == "Active"
        SHOW "Account is " + Status
    ORIF Status == "Low Balance"
        SHOW "Warning: " + Status
    IFNOT
        SHOW "Alert: " + Status
    ENDIF

    A1 = 0
    SHOW "Reset balance: $" + A1

    CLEANALL
ENDWHEN
```

---

## 10. VMA Lifetime Reference (updated)

| Operation | Effect |
|---|---|
| `VAR NUM X EAQ 10` | Allocates VMA, binds `X`, sets value |
| `CLEAN A1` | Sets `Allocated = false`, clears identifier binding |
| `CLEAN X` | Same as `CLEAN A1` (identifier resolves to VMA) |
| `AUTOCLEAN ON` | Auto-clean when enclosing `WHEN` block / program exits |
| `CLEANALL` | Frees all VMAs immediately |
| `VAR NUM X EAQ A1` | ❌ Error: cannot alias an already-allocated VMA |
| `X = 20` | Updates value at `X`'s VMA |
| `A1 = 20` | Updates value at `A1` directly |
| `LOAD A1` | Loads value from `A1` |
| `STORE 20 A1` | Stores `20` into `A1` |

---

## 11. Changelog

### v1.0 → v1.1

| # | v1.0 issue | v1.1 resolution |
|---|---|---|
| 1 | Allocation order contradicted examples (bare letters vs. numeric form) | Always allocate numeric form `A1, A2, ...`; bare letters removed from the scheme |
| 2 | RefCount could show values >1 despite aliasing being banned | Replaced with boolean `Allocated` flag; refcounting removed |
| 3 | `AUTOCLEAN`'s "out of scope" was undefined | Scope = `WHEN` block or top-level program; `IF`/`WHILE`/`FOR` don't introduce scope |
| 4 | Undefined behavior when multiple `WHEN` conditions become true at once | All matching handlers fire, in declaration order |
| 5 | STORE/LOAD argument order was implicit from examples only | Formalized: `STORE <value\|identifier> <VMA>`, `LOAD <VMA>` |
| 6 | Event handler re-entrancy (a handler mutating a watched VMA) was unspecified | Queued FIFO dispatch, breadth-first, max-depth guarded against infinite loops |

### v1.1 → v1.2

| # | v1.1 issue | v1.2 resolution |
|---|---|---|
| 7 | Unclear whether `WHEN <condition>` fires on every assignment where it holds ("level-triggered") or only on the transition into holding ("edge-triggered") | Edge-triggered, formalized via per-handler `last_state`; fires only on false→true crossing |
| 8 | `WHEN <VMA> CHANGED` didn't define what counts as "changed" (e.g. `A1 = A1`, or reassigning the same value) | Fires iff `new_value != old_value`, compared against the VMA table's stored value |

---

## 12. Official Description

Virtual Order is a VMA-oriented, assembly-inspired programming language combining explicit runtime memory areas with structured control flow, typed variables, collections, events, arithmetic, logical operations, and direct memory manipulation.

Variables are associated with Virtual Memory Areas (VMAs) using the `EAQ` (Establish and Allocate) operator, allowing programs to operate through human-readable variable names or directly through their underlying VMA identifiers.

Core features:
- Two programming models: variable-oriented and VMA-oriented
- Memory-level events: react to changes in specific VMAs, dispatched via a FIFO event queue
- Strong typing: `NUM`, `DEC`, `TEX`, `YN`, `COLL`
- Automatic memory management: scope-based `AUTOCLEAN`, no aliasing, no refcounting
- Direct VMA manipulation alongside named identifiers
- Structured control flow: `IF`/`ORIF`/`IFNOT`, `FOR`, `WHILE`, `GOTO`
- Rich operator set: arithmetic, comparison, logical, bitwise, assignment
