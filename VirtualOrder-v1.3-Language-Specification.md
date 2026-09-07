# Virtual Order v1.3 — Language Specification

*Revision notes: v1.3 introduces the Virtual Order module system, command-oriented terminology, the `EMP` (Empty) data type, and `GIVE` for returning values from `JOB`s. `HARD` is the official Virtual Order keyword for immutable values; the conventional keyword `CONST` is not part of the language.*

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

`=` is assignment.

`==` is equality comparison.

---

# 2. Core Virtual Order Vocabulary

| Conventional Concept | Virtual Order Keyword | Meaning |
|---|---|---|
| Module | `PEICE` | Defines a self-contained piece of Virtual Order code |
| Function | `JOB` | Defines a reusable operation |
| Export | `SHIP` | Makes a module member available externally |
| Constant | `HARD` | Defines an immutable value |
| Return | `GIVE` | Gives a value back from a `JOB` |
| Assert | `DEMAND` | Requires a condition to be true |
| Error | `ISSUE` | Represents an execution or logical error |
| Throw | `SERVE` | Raises/serves an issue |
| Try | `DO` | Begins protected execution |
| Catch | `GRABE` | Receives and handles a served issue |
| Import | `BRING` | Loads another `.vo` piece |

The conventional keywords represented in this table are **not** aliases. Virtual Order uses the Virtual Order terminology as its official syntax.

---

# 3. Data Types

Virtual Order v1.3 defines the following built-in data types:

| Type | Name | Description |
|---|---|---|
| `NUM` | Number | Integer numeric value |
| `DEC` | Decimal | Floating-point numeric value |
| `TEX` | Text | Text/string value |
| `YN` | Yes/No | Boolean value |
| `COLL` | Collection | Ordered collection of values |
| `EMP` | Empty | Explicit absence of a value |

## A. EMP — Empty

`EMP` represents an explicitly empty value.

```asm
VAR EMP Nothing EAQ EMP
```

`EMP` is a valid runtime value and a valid VMA type.

An `EMP` value is **not** equivalent to an unallocated VMA.

```text
VMA contains EMP
    = VMA exists but contains no value

VMA is unallocated
    = VMA does not currently exist as usable memory
```

Example:

```asm
VAR NUM Score EAQ 100
Score = EMP
```

`Score` still owns its VMA. The VMA now contains `EMP`.

## B. EMP and Boolean Conversion

```text
EMP → NO
```

Example:

```asm
VAR EMP Value EAQ EMP

IF Value
    SHOW "Has value"
ENDIF
```

The `IF` body does not execute.

## C. EMP Comparisons

Equality and inequality comparisons with `EMP` are legal:

```asm
IF Value == EMP
    SHOW "Value is empty"
ENDIF
```

```asm
IF Value != EMP
    SHOW "Value contains something"
ENDIF
```

## D. EMP and Operations

Operations requiring a concrete value cannot operate on `EMP`.

```asm
EMP + 5
```

is invalid and produces an `ISSUE`.

---

# 4. Conditional Constructs

## A. Standard IF chain

```asm
IF condition1
    statements

ORIF condition2
    statements

IFNOT
    statements

ENDIF
```

`IFNOT` with no expression is the `ELSE` branch.

## B. IFNOT with a condition

```asm
IFNOT A1 > 0
    SHOW "A1 is zero or negative"
ENDIF
```

This is equivalent to:

```asm
IF NOT (A1 > 0)
    SHOW "A1 is zero or negative"
ENDIF
```

The parser disambiguates the forms by the presence or absence of a trailing expression.

`IF`, `ORIF`, `IFNOT`, `WHILE`, and `FOR` bodies do **not** introduce a variable scope.

---

# 5. Increment / Decrement Rules

Increment and decrement are statement-only operations.

```asm
Counter++
A1--
```

They cannot be used inside expressions.

Invalid:

```asm
Result = Counter++ * 5
IF A1++ > 0
```

Using increment or decrement on an `EMP` value produces an `ISSUE`.

---

# 6. Boolean Conversion Rules

| Context | Rule |
|---|---|
| `NUM → YN` | `0` = `NO`, nonzero = `YES` |
| `DEC → YN` | `0.0` = `NO`, nonzero = `YES` |
| `TEX → YN` | `""` = `NO`, non-empty = `YES` |
| `COLL → YN` | `[]` = `NO`, non-empty = `YES` |
| `EMP → YN` | Always `NO` |

---

# 7. Identifier ↔ VMA Equivalence

```asm
VAR NUM Temp EAQ 25
```

If `Temp` receives VMA `A1`, then:

```asm
Temp = 30
```

has the same effect as:

```asm
A1 = 30
```

Rules:

- A VMA binding remains stable for the lifetime of the identifier.
- `CLEAN A1` invalidates the corresponding identifier binding.
- An identifier can never be rebound to another VMA.
- Aliasing is forbidden in v1.3.
- A `VAR` declaration may not claim an already-allocated VMA.
- An identifier may hold `EMP` while its VMA remains allocated.

---

# 8. VMA Formal Specification

## A. Addressing Scheme

```text
VMA ::= [A-Z]+[0-9]+
```

Allocation order:

```text
A1, A2, ..., A9999,
B1, B2, ..., Z9999,
AA1, AA2, ..., ZZ9999,
AAA1, AAA2, ...
```

Bare letters are not VMAs.

## B. Allocation Strategy

Allocation uses first-fit over the defined address order.

Freed VMAs are reused before advancing the allocation frontier.

```asm
VAR NUM X EAQ 10
VAR NUM Y EAQ 20

CLEAN A2

VAR NUM Z EAQ 30
```

Result:

```text
X → A1
Z → A2
```

## C. VMA Table

A runtime VMA table contains at least:

```text
┌─────────┬──────────┬─────────────┬───────────┬────────────┐
│ VMA     │ Type     │ Value       │ Allocated │ Identifier │
├─────────┼──────────┼─────────────┼───────────┼────────────┤
│ A1      │ NUM      │ 45          │ true      │ Age        │
│ A2      │ TEX      │ "John"      │ true      │ Name       │
│ B1      │ NUM      │ 10          │ true      │ Counter    │
│ B2      │ COLL     │ [1,2,3]     │ true      │ Scores     │
│ B3      │ EMP      │ EMP         │ true      │ Nothing    │
│ B4      │ —        │ —           │ false     │ -          │
└─────────┴──────────┴─────────────┴───────────┴────────────┘
```

`EMP` and an unallocated VMA are fundamentally different states.

### Allocated EMP

```text
Allocated = true
Type = EMP
Value = EMP
```

### Unallocated VMA

```text
Allocated = false
Type = —
Value = —
```

Because aliasing is forbidden, a VMA has at most one identifier owner.

---

# 9. VMA Lifetime

## A. Manual Cleanup

```asm
CLEAN A1
CLEAN X
```

Both forms free the corresponding VMA.

Cleaning a VMA containing `EMP` still frees the VMA.

## B. Automatic Cleanup

```asm
AUTOCLEAN ON
AUTOCLEAN OFF
```

When enabled, variables are automatically cleaned when their enclosing scope exits.

## C. Global Cleanup

```asm
CLEANALL
```

Frees all allocated VMAs and resets runtime VMA state.

## D. Invalid Access

Accessing a freed VMA produces:

```text
VMA A1 is not allocated
```

This is distinct from accessing an allocated VMA whose value is `EMP`.

---

# 10. VMA Events

Virtual Order provides event-driven execution using `WHEN`.

## A. Changed Event

```asm
WHEN A1 CHANGED
    SHOW "A1 changed"
ENDWHEN
```

`CHANGED` fires iff:

```text
new_value != old_value
```

`EMP` participates in this comparison as a normal value.

Example:

```asm
A1 = EMP
A1 = EMP
```

does not fire `CHANGED`.

```asm
A1 = 10
```

does fire because:

```text
EMP != 10
```

`OLD_VALUE` and `NEW_VALUE` are implicit read-only bindings inside a `CHANGED` handler.

## B. Conditional Event

```asm
WHEN A1 > 100
    SHOW "Threshold reached"
ENDWHEN
```

Conditional `WHEN` handlers are edge-triggered.

Each handler stores an internal `last_state` initialized to `false` when registered.

On each assignment affecting a referenced VMA:

1. Evaluate the condition.
2. Store the result as `current_state`.
3. If `last_state == false` and `current_state == true`, enqueue the handler.
4. If `last_state == true` and `current_state == false`, re-arm the handler.
5. Otherwise, do nothing.
6. Update `last_state`.

## C. Simultaneous Triggers

If multiple handlers become true because of one assignment, all matching handlers are queued in declaration order.

## D. Event Re-entrancy

Events are queued rather than recursively executed.

The queue is FIFO.

A handler producing another event appends that event to the back of the queue.

Default maximum queue depth per program tick:

```text
1000
```

Exceeding this produces:

```text
Event queue overflow: possible infinite trigger loop
```

---

# 11. STORE / LOAD

## Grammar

```text
STORE ::= "STORE" (value | identifier) VMA
LOAD  ::= "LOAD" VMA
```

Examples:

```asm
STORE 20 A1
STORE Age A1
LOAD A1
```

`STORE` writes to the destination VMA.

`LOAD` retrieves the stored value.

`EMP` is also a valid value for `STORE`:

```asm
STORE EMP A1
```

---

# 12. Functions — JOB

A reusable operation is a `JOB`.

```asm
JOB ADD NUM A NUM B
    GIVE A + B
ENDJOB
```

Invocation:

```asm
SHOW ADD(10, 5)
```

A `JOB` may receive parameters and may `GIVE` a value to its caller.

## A. GIVE

`GIVE` immediately returns a value from the current `JOB`.

```asm
JOB SQUARE NUM A
    GIVE A * A
ENDJOB
```

A `JOB` may also give an identifier's value:

```asm
JOB GETBALANCE
    GIVE Balance
ENDJOB
```

A `GIVE` statement outside a `JOB` is invalid.

A `JOB` that reaches `ENDJOB` without executing `GIVE` has no returned value; the exact implicit result is `EMP`.

---

# 13. Immutable Values — HARD

`HARD` defines an immutable value.

```asm
HARD NUM MAX_BALANCE = 100000
HARD TEX VERSION = "1.3"
```

Assignment after declaration is forbidden.

```asm
MAX_BALANCE = 200000
```

is a compile-time error.

`CONST` is **not** a Virtual Order keyword.

`HARD` is the official immutable-value construct.

---

# 14. Conditions — DEMAND

`DEMAND` requires a condition to evaluate to `YES`.

```asm
DEMAND Balance >= 0
```

If false, an `ISSUE` is produced.

An optional message may be supplied:

```asm
DEMAND Balance >= 0 "Balance cannot be negative"
```

Example:

```asm
DEMAND Name != EMP "Name is required"
```

---

# 15. Issues — ISSUE and SERVE

## A. ISSUE

An `ISSUE` represents an error condition.

Examples include:

```text
VMA A1 is not allocated
Type mismatch
Module not found
Circular module dependency
Invalid assignment
Operation requires a non-EMP value
```

## B. SERVE

`SERVE` raises an issue during execution.

```asm
SERVE "Balance cannot be negative"
```

The served issue enters the active issue-handling mechanism.

---

# 16. Protected Execution — DO / GRABE

Protected execution uses:

```asm
DO
    statements

GRABE
    statements

ENDDO
```

If execution inside the `DO` block serves an `ISSUE`, control transfers to the `GRABE` section.

Example:

```asm
DO
    SERVE "Something went wrong"

GRABE
    SHOW "An issue was received"

ENDDO
```

---

# 17. Modules — PEICE

A `PEICE` is a self-contained source unit.

```asm
PEICE mathlib

JOB ADD NUM A NUM B
    GIVE A + B
ENDJOB

ENDPEICE
```

A PEICE normally resides in a `.vo` file:

```text
mathlib.vo
```

The PEICE name must match the filename.

---

# 18. Bringing Modules — BRING

A `.vo` file is loaded using:

```asm
BRING mathlib.vo
```

Exported members become available through the module namespace.

Example:

```asm
SHOW mathlib.ADD(10, 5)
```

`BRING` does not merge all module identifiers into the current namespace.

---

# 19. Module Namespaces

Each PEICE receives an independent namespace.

```asm
BRING mathlib.vo
BRING stringlib.vo
```

Members are accessed as:

```asm
mathlib.ADD(...)
stringlib.LENGTH(...)
```

This prevents naming collisions.

---

# 20. Shipping Members — SHIP

Only members explicitly marked with `SHIP` are externally accessible.

```asm
PEICE mathlib

SHIP ADD
SHIP SUB

JOB ADD NUM A NUM B
    GIVE A + B
ENDJOB

JOB SUB NUM A NUM B
    GIVE A - B
ENDJOB

JOB INTERNAL_HELPER NUM X
    GIVE X * 2
ENDJOB

ENDPEICE
```

`INTERNAL_HELPER` remains private.

---

# 21. Module VMA Isolation

VMAs declared inside a PEICE are private to that PEICE by default.

Example:

```asm
PEICE bank

VAR NUM Balance EAQ 1000

JOB DEPOSIT NUM Amount
    Balance += Amount
    GIVE Balance
ENDJOB

SHIP DEPOSIT

ENDPEICE
```

External code may call:

```asm
bank.DEPOSIT(500)
```

but cannot directly access:

```asm
bank.Balance
```

unless a future explicit VMA-export feature is introduced.

---

# 22. Module Initialization

A PEICE is initialized the first time it is brought into the program.

Repeated `BRING` operations do not initialize it again.

The runtime maintains a loaded-module registry.

---

# 23. Module Resolution

v1.3 module resolution searches:

1. Directory containing the current source file
2. Project module directory
3. Standard library/module directory

Examples:

```asm
BRING mathlib.vo
BRING libs/mathlib.vo
BRING game/entities.vo
```

---

# 24. Duplicate Modules

A PEICE may only be loaded once during a program execution.

Repeated `BRING` statements refer to the already-loaded PEICE.

---

# 25. Circular Module Dependencies

Circular dependencies are illegal.

Example:

```text
a.vo → b.vo → a.vo
```

produces an `ISSUE` such as:

```text
Circular module dependency: a -> b -> a
```

Modules must not be partially initialized to resolve circular dependencies.

---

# 26. Complete Module Example

### `mathlib.vo`

```asm
PEICE mathlib

SHIP ADD
SHIP SUB
SHIP SQUARE

HARD TEX VERSION = "1.3"

JOB ADD NUM A NUM B
    GIVE A + B
ENDJOB

JOB SUB NUM A NUM B
    GIVE A - B
ENDJOB

JOB SQUARE NUM A
    GIVE A * A
ENDJOB

JOB INTERNAL_HELPER NUM X
    GIVE X * 2
ENDJOB

ENDPEICE
```

### `main.vo`

```asm
BRING mathlib.vo

SHOW "Math Library Version: " + mathlib.VERSION

SHOW mathlib.ADD(10, 5)
SHOW mathlib.SUB(10, 5)
SHOW mathlib.SQUARE(6)
```

---

# 27. Comprehensive Example

```asm
BRING mathlib.vo

PEICE bank

SHIP DEPOSIT
SHIP WITHDRAW

VAR NUM Balance EAQ 1000
VAR NUM Transaction EAQ 0
VAR TEX Status EAQ "Active"
VAR COLL History EAQ []
VAR EMP LastIssue EAQ EMP

HARD NUM MIN_BALANCE = 0

AUTOCLEAN ON

WHEN Balance CHANGED
    History += NEW_VALUE
    SHOW "Balance: $" + Balance
ENDWHEN

WHEN Balance < 100
    Status = "Low Balance"
    SHOW "Low Balance - Please deposit"
ENDWHEN

WHEN Balance < 0
    Status = "Overdrawn!"
    SHOW "ACCOUNT OVERDRAWN!"
ENDWHEN

JOB DEPOSIT NUM Amount
    DEMAND Amount > 0 "Deposit must be greater than zero"

    Transaction = Amount
    Balance += Transaction

    GIVE Balance
ENDJOB

JOB WITHDRAW NUM Amount
    DEMAND Amount > 0 "Withdrawal must be greater than zero"

    Transaction = -Amount
    Balance += Transaction

    GIVE Balance
ENDJOB

JOB CHECK
    DEMAND Balance >= MIN_BALANCE "Account is overdrawn"
    GIVE Balance
ENDJOB

ENDPEICE
```

A program using it:

```asm
BRING bank.vo

SHOW bank.DEPOSIT(250)
SHOW bank.WITHDRAW(100)

DO
    SHOW bank.CHECK()

GRABE
    SHOW "Bank operation failed"

ENDDO
```

---

# 28. Language Architecture

```text
Virtual Order Source Code
        │
        ▼
     Lexer
        │
        ▼
     Parser
        │
        ▼
      AST
        │
        ▼
    Analyzer
    ├── Type checking
    ├── Symbol resolution
    ├── Scope checking
    ├── Module resolution
    ├── JOB validation
    ├── SHIP validation
    └── EMP validation
        │
        ▼
   Module Manager
    ├── BRING resolution
    ├── PEICE loading
    ├── dependency tracking
    ├── circular dependency detection
    └── initialization
        │
        ▼
    VMA Table
        │
        ▼
   Event Queue
        │
        ▼
     Runtime
        │
        ▼
      Output
```

---

# 29. VMA Lifetime Reference

| Operation | Effect |
|---|---|
| `VAR NUM X EAQ 10` | Allocates a VMA and binds `X` |
| `VAR EMP X EAQ EMP` | Allocates a VMA containing `EMP` |
| `CLEAN A1` | Frees `A1` |
| `CLEAN X` | Frees the VMA belonging to `X` |
| `AUTOCLEAN ON` | Enables automatic cleanup |
| `AUTOCLEAN OFF` | Disables automatic cleanup |
| `CLEANALL` | Frees all VMAs |
| `X = 20` | Updates the VMA belonging to `X` |
| `X = EMP` | Keeps the VMA allocated but replaces its value with `EMP` |
| `A1 = 20` | Updates VMA `A1` |
| `LOAD A1` | Loads the value from `A1` |
| `STORE 20 A1` | Stores `20` into `A1` |
| `STORE EMP A1` | Stores `EMP` into `A1` |

---

# 30. Data Type Reference

| Type | Meaning | Empty/False Representation |
|---|---|---|
| `NUM` | Integer number | `0` |
| `DEC` | Decimal number | `0.0` |
| `TEX` | Text | `""` |
| `YN` | Yes/No | `NO` |
| `COLL` | Collection | `[]` |
| `EMP` | Empty | `EMP` |

---

# 31. Module and Control Keyword Reference

| Keyword | Purpose |
|---|---|
| `PEICE` | Declares a module |
| `ENDPEICE` | Ends a module |
| `BRING` | Loads a `.vo` module |
| `SHIP` | Exports a module member |
| `JOB` | Declares a reusable operation |
| `ENDJOB` | Ends a job |
| `GIVE` | Returns a value from a job |
| `HARD` | Declares an immutable value |
| `DEMAND` | Requires a condition to be true |
| `ISSUE` | Represents an error |
| `SERVE` | Raises an issue |
| `DO` | Begins protected execution |
| `GRABE` | Handles a served issue |
| `ENDDO` | Ends protected execution |

---

# 32. Reserved-Word Changes

The following conventional keywords are **not** part of Virtual Order v1.3:

```text
CONST
RETURN
MODULE
FUNC
EXPORT
ASSERT
ERROR
THROW
TRY
CATCH
```

Their Virtual Order equivalents are:

```text
CONST  → HARD
RETURN → GIVE
MODULE → PEICE
FUNC   → JOB
EXPORT → SHIP
ASSERT → DEMAND
ERROR  → ISSUE
THROW  → SERVE
TRY    → DO
CATCH  → GRABE
```

---

# 33. Changelog

## v1.2 → v1.3

| # | Change | Resolution |
|---|---|---|
| 9 | No standardized module/import system | Added `BRING` |
| 10 | No module boundary | Added `PEICE` |
| 11 | No public/private module members | Added `SHIP` |
| 12 | No reusable function terminology | Added `JOB` / `ENDJOB` |
| 13 | Conventional constant keyword conflicted with existing usage | Added `HARD` |
| 14 | No return-value command | Added `GIVE` |
| 15 | No assertion construct | Added `DEMAND` |
| 16 | No standardized runtime issue terminology | Added `ISSUE` |
| 17 | No standardized issue raising terminology | Added `SERVE` |
| 18 | No protected execution syntax | Added `DO` / `GRABE` / `ENDDO` |
| 19 | No module namespace isolation | Added module-qualified member access |
| 20 | No module initialization rule | Added once-per-execution initialization |
| 21 | No circular dependency rule | Circular module dependencies are errors |
| 22 | Module VMA ownership was undefined | Module VMAs are private by default |
| 23 | No explicit empty/null value type | Added `EMP` |
| 24 | `NULL` and unallocated VMAs were ambiguous | `EMP` represents an allocated VMA containing no value |
| 25 | Empty-value behavior was unspecified | Defined `EMP` conversion, comparison, assignment, and invalid operations |
| 26 | `RETURN` conflicted with Virtual Order terminology | Replaced with `GIVE` |

---

# 34. Official Description

Virtual Order is a VMA-oriented, assembly-inspired programming language combining explicit runtime memory areas with structured control flow, typed variables, collections, events, reusable jobs, modular code, arithmetic, logical operations, issue handling, and direct memory manipulation.

Variables are associated with Virtual Memory Areas using the `EAQ` (Establish and Allocate) operator, allowing programs to operate through human-readable identifiers or directly through their underlying VMA identifiers.

Virtual Order provides two complementary programming models:

```text
Variable-oriented programming
        +
VMA-oriented programming
```

Its defining features include:

- Explicit Virtual Memory Areas
- Stable identifier-to-VMA bindings
- Strongly typed values
- `NUM`, `DEC`, `TEX`, `YN`, `COLL`, and `EMP`
- Explicit empty values through `EMP`
- Distinction between empty and unallocated memory
- Automatic and manual memory cleanup
- Direct VMA access
- Memory-level events
- FIFO event dispatch
- Edge-triggered conditional events
- Structured control flow
- Reusable `JOB`s
- Value return through `GIVE`
- Immutable `HARD` values
- Runtime validation through `DEMAND`
- Issue handling through `DO`, `SERVE`, and `GRABE`
- Modular programming through `PEICE`
- Module loading through `BRING`
- Explicit public interfaces through `SHIP`
- Module namespace isolation

Virtual Order is built around a simple principle:

> **The programmer gives the order. The Virtual Memory Areas obey.**