# Virtual Order v1.4 — Language Specification

*Revision notes: v1.4 expands Virtual Order with collection and string operations, TEX interpolation, file I/O, concurrency, and advanced event control. v1.4 preserves the VMA model, module system, `EMP`, `JOB`/`GIVE`, `HARD`, and the command-oriented terminology established in v1.3. Generic programming terminology is not reserved when Virtual Order already has its own official command.*

---

# 1. Operator Precedence Table

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

# 2. Comments

Virtual Order supports single-line and multiline comments.

## A. Single-Line Comments

A single semicolon `;` begins a comment that continues to the end of the current line.

```asm
; This is a comment
SHOW "Hello"
```

Comments may follow executable code:

```asm
SHOW "Hello" ; This is also a comment
```

## B. Multiline Comments

A double semicolon `;;` begins a multiline comment.

The next `;;` closes the comment.

```asm
;;
This is a multiline comment.

It may contain as many
lines as necessary.
;;

SHOW "Hello"
```

Multiline comments may appear between statements.

```asm
VAR NUM Age EAQ 18

;;
Explain why the age
is being checked here.
;;

IF Age >= 18
    SHOW "Adult"
ENDIF
```

`#` is not a Virtual Order comment marker.

---

# 3. Core Virtual Order Vocabulary

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

These are the official Virtual Order terms. They are not aliases for conventional keywords.

---

# 4. Data Types

Virtual Order v1.4 defines the following built-in types:

| Type | Name | Description |
|---|---|---|
| `NUM` | Number | Integer numeric value |
| `DEC` | Decimal | Floating-point numeric value |
| `YN` | Yes/No | Boolean value |
| `TEX` | Text | String/text value |
| `COLL` | Collection | Ordered collection of values |
| `EMP` | Empty | Explicit absence of a value |
| `FILE` | File | Open file resource |
| `TASK` | Task | Concurrent execution handle |
| `LOCK` | Lock | Synchronization resource |
| `EVENT` | Event | Event handler resource |

---

# 5. EMP — Empty

`EMP` represents an explicitly empty value.

```asm
VAR EMP Nothing EAQ EMP
```

`EMP` is a valid runtime value and a valid VMA type.

An `EMP` VMA is different from an unallocated VMA.

```text
Allocated VMA containing EMP
    = VMA exists and is usable

Unallocated VMA
    = VMA does not exist as usable memory
```

Example:

```asm
VAR NUM Score EAQ 100

Score = EMP
```

The VMA remains allocated.

Only its stored value changes to `EMP`.

## A. EMP and Boolean Conversion

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

The body does not execute.

## B. EMP Comparisons

Equality and inequality comparisons with `EMP` are legal.

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

## C. EMP and Operations

Operations requiring a concrete value cannot operate on `EMP`.

```asm
EMP + 5
```

is invalid and produces an `ISSUE`.

---

# 6. Variable Declaration

Variables are declared using:

```text
VAR TYPE Identifier EAQ Value
```

Examples:

```asm
VAR NUM Age EAQ 18
VAR DEC Temperature EAQ 23.5
VAR YN Alive EAQ YES
VAR TEX Name EAQ "Alex"
VAR COLL Numbers EAQ [1, 2, 3]
VAR EMP Nothing EAQ EMP
```

`EAQ` establishes the variable and allocates a VMA for it.

---

# 7. Input and Output

## A. SHOW

`SHOW` displays one or more values.

```asm
SHOW "Hello"
SHOW Age
SHOW A1
```

Multiple values may be displayed:

```asm
SHOW "Age:", Age
```

## B. TAKE

`TAKE` obtains user input.

```asm
TAKE("Write your input here: ")
```

The result is an expression value and may be assigned:

```asm
VAR TEX Name EAQ TAKE("Enter your name: ")
```

`TAKE` is the official user-input command.

`TAKE` is not a file-reading command.

---

# 8. Assignment

Assignment uses:

```asm
X = Value
```

Compound assignment:

```asm
X += Value
X -= Value
X *= Value
X /= Value
X %= Value
```

Assignment to a collection element is also permitted:

```asm
Items[1] = "Changed"
```

Assignment replaces the value stored at the target VMA or collection position.

---

# 9. Increment / Decrement Rules

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

Incrementing or decrementing `EMP` produces an `ISSUE`.

---

# 10. Boolean Conversion Rules

| Context | Rule |
|---|---|
| `NUM → YN` | `0` = `NO`, nonzero = `YES` |
| `DEC → YN` | `0.0` = `NO`, nonzero = `YES` |
| `TEX → YN` | `""` = `NO`, non-empty = `YES` |
| `COLL → YN` | `[]` = `NO`, non-empty = `YES` |
| `EMP → YN` | Always `NO` |

---

# 11. Identifier ↔ VMA Equivalence

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
- An identifier cannot be rebound to another VMA.
- Aliasing is forbidden.
- A `VAR` declaration may not claim an already-allocated VMA.
- An identifier may hold `EMP` while its VMA remains allocated.

---

# 12. VMA Formal Specification

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

Example:

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

Because aliasing is forbidden, a VMA has at most one identifier owner.

---

# 13. VMA Lifetime

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

When enabled, variables are automatically cleaned when their enclosing lifetime scope exits.

## C. Global Cleanup

```asm
CLEANALL
```

Frees all allocated VMAs and resets VMA runtime state.

## D. Invalid Access

Accessing a freed VMA produces an `ISSUE`.

Example:

```text
VMA A1 is not allocated
```

This is distinct from an allocated VMA containing `EMP`.

---

# 14. Conditional Constructs

## A. IF

```asm
IF condition
    statements
ENDIF
```

## B. ORIF

```asm
IF condition1
    statements
ORIF condition2
    statements
ENDIF
```

## C. IFNOT

`IFNOT` without an expression acts as the final `ELSE` branch.

```asm
IF Age >= 18
    SHOW "Adult"
IFNOT
    SHOW "Minor"
ENDIF
```

`IFNOT` may also contain a condition:

```asm
IFNOT Age > 0
    SHOW "Age is zero or negative"
ENDIF
```

This is equivalent to:

```asm
IF NOT (Age > 0)
    SHOW "Age is zero or negative"
ENDIF
```

The parser distinguishes the two forms by the presence or absence of an expression.

`IF`, `ORIF`, `IFNOT`, `WHILE`, and `FOR` bodies do not introduce variable scope.

---

# 15. WHILE

`WHILE` repeatedly executes a block while its condition evaluates to `YES`.

```asm
WHILE Counter < 10
    Counter++
ENDWHILE
```

The condition is evaluated before every iteration.

If the condition is initially `NO`, the body does not execute.

---

# 16. FOR

`FOR` performs counted iteration.

```asm
FOR I = 1 TO 10
    SHOW I
ENDFOR
```

The loop variable receives each value in sequence.

The `FOR` body does not introduce a new variable scope.

---

# 17. STORE / LOAD

## A. STORE

```text
STORE value VMA
```

Examples:

```asm
STORE 20 A1
STORE Age A1
STORE EMP A1
```

`STORE` writes a value into the target VMA.

## B. LOAD

```asm
LOAD A1
```

`LOAD` retrieves the current value stored at a VMA.

Example:

```asm
SHOW LOAD A1
```

Using an unallocated VMA produces an `ISSUE`.

---

# 18. Jobs — JOB

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

A `JOB` may accept typed parameters.

A `JOB` executes in its own local execution context.

---

# 19. GIVE

`GIVE` returns a value from a `JOB`.

```asm
JOB SQUARE NUM A
    GIVE A * A
ENDJOB
```

`GIVE` immediately ends the current `JOB` execution and returns its value to the caller.

A `GIVE` outside a `JOB` is invalid.

If execution reaches `ENDJOB` without executing `GIVE`, the job produces `EMP`.

---

# 20. Immutable Values — HARD

`HARD` defines an immutable value.

```asm
HARD NUM MAX_BALANCE = 100000
HARD TEX VERSION = "1.4"
```

After initialization, assignment is forbidden.

```asm
MAX_BALANCE = 200000
```

is a compile-time error.

`CONST` is not a Virtual Order keyword.

`HARD` is the official immutable-value construct.

---

# 21. Conditions — DEMAND

`DEMAND` requires a condition to evaluate to `YES`.

```asm
DEMAND Balance >= 0
```

A failed demand produces an `ISSUE`.

An optional message may be supplied:

```asm
DEMAND Balance >= 0 "Balance cannot be negative"
```

---

# 22. Issues — ISSUE and SERVE

## A. ISSUE

An `ISSUE` represents an error condition.

Examples:

```text
VMA A1 is not allocated
Type mismatch
Module not found
Circular module dependency
Invalid assignment
Invalid collection index
Invalid file operation
Operation requires a non-EMP value
```

## B. SERVE

`SERVE` raises an issue.

```asm
SERVE "Balance cannot be negative"
```

The issue enters the active issue-handling mechanism.

---

# 23. Protected Execution — DO / GRABE

Protected execution uses:

```asm
DO
    statements

GRABE
    statements

ENDDO
```

If an `ISSUE` is served inside the `DO` block, execution transfers to `GRABE`.

Example:

```asm
DO
    SERVE "Something went wrong"

GRABE
    SHOW "Recovered"

ENDDO
```

---

# 24. Collections — COLL

A `COLL` is an ordered collection of values.

Example:

```asm
VAR COLL Items EAQ ["A", "B", "C"]
```

Collections may contain values of different types:

```asm
VAR COLL Data EAQ [10, "hello", YES, EMP]
```

Collections are zero-indexed.

---

# 25. Collection Indexing

Indexes begin at `0`.

```asm
SHOW Items[0]
SHOW Items[1]
SHOW Items[2]
```

Elements may be assigned:

```asm
Items[1] = "Changed"
```

An invalid index produces an `ISSUE`.

---

# 26. Collection Slicing

Slices use:

```text
Collection[Start:End]
```

The start index is inclusive.

The end index is exclusive.

Example:

```asm
SHOW Items[1:4]
```

For:

```text
[0, 1, 2, 3, 4]
```

the result is:

```text
[1, 2, 3]
```

Invalid slice boundaries produce an `ISSUE`.

---

# 27. Collection Commands

Virtual Order uses command-oriented names for collection operations.

## A. ATTACH

Adds a value to the end of a collection.

```asm
ATTACH Items, "New"
```

## B. PLACE

Inserts a value at a specific index.

```asm
PLACE Items, 1, "Inserted"
```

## C. ERASE

Removes a collection element.

```asm
ERASE Items, 2
```

`ERASE` is also used for filesystem deletion.

## D. COUNT

Returns the number of elements in a collection.

```asm
SHOW COUNT(Items)
```

`COUNT` also operates on `TEX`.

## E. SEEK

Searches for a value or substring.

```asm
SEEK(Items, "Alex")
SEEK("Hello World", "World")
```

For collections, `SEEK` returns the matching index.

For text, `SEEK` returns the position of the matching substring.

When no match exists, the result is `EMP`.

## F. HAS

Checks whether a collection contains a value or text contains a substring.

```asm
HAS(Items, "Alex")
HAS("Hello World", "World")
```

Result:

```text
YES
NO
```

## G. BIND

Joins collection values into text using a separator.

```asm
BIND(Items, ", ")
```

Example:

```text
["A", "B", "C"]
```

becomes:

```text
"A, B, C"
```

## H. SEVER

Splits text into a collection.

```asm
SEVER("A,B,C", ",")
```

Result:

```text
["A", "B", "C"]
```

## I. CUT

Removes surrounding whitespace from text.

```asm
CUT(Text)
```

## J. RAISE

Converts text to uppercase.

```asm
RAISE(Text)
```

## K. LOWER

Converts text to lowercase.

```asm
LOWER(Text)
```

---

# 28. Text — TEX

`TEX` stores text values.

```asm
VAR TEX Message EAQ "Hello"
```

Text is zero-indexed.

```asm
Message[0]
```

accesses its first character.

Text may also be sliced:

```asm
Message[1:5]
```

Text indexing returns text values representing individual characters.

An invalid index produces an `ISSUE`.

---

# 29. TEX Interpolation

Virtual Order supports expression interpolation inside `TEX` literals.

An interpolation expression is enclosed in `{` and `}`.

Example:

```asm
VAR TEX Name EAQ "Alex"
VAR NUM Age EAQ 18

SHOW "Name: {Name}, Age: {Age}"
```

The result is:

```text
Name: Alex, Age: 18
```

## A. Expression Interpolation

Interpolation may contain any valid expression.

```asm
SHOW "Next year: {Age + 1}"
```

```asm
SHOW "Total: {Price * Quantity}"
```

```asm
SHOW "Result: {ADD(10, 5)}"
```

The expression is evaluated when the text value is created.

## B. VMA Interpolation

A VMA may be interpolated directly:

```asm
SHOW "Value: {A1}"
```

This is equivalent to converting the current value stored in `A1` to text.

## C. Nested Expressions

Parentheses may be used normally inside interpolation:

```asm
SHOW "Result: {(A + B) * 2}"
```

## D. Escaping Braces

A literal `{` is represented by:

```text
{{
```

A literal `}` is represented by:

```text
}}
```

Example:

```asm
SHOW "Use {{Name}} to represent a placeholder."
```

produces:

```text
Use {Name} to represent a placeholder.
```

Escaped braces are not evaluated as interpolation.

## E. Interpolation Conversion

Interpolated values are converted to text according to their normal textual representation.

| Type | Interpolated representation |
|---|---|
| `NUM` | Numeric representation |
| `DEC` | Decimal representation |
| `YN` | `YES` or `NO` |
| `TEX` | Text itself |
| `COLL` | Standard collection representation |
| `EMP` | `EMP` |

Example:

```asm
VAR EMP Value EAQ EMP

SHOW "Value: {Value}"
```

produces:

```text
Value: EMP
```

`EMP` does not silently become an empty string.

## F. Invalid Interpolation

Malformed interpolation produces a compile-time parsing error.

Examples include:

```asm
SHOW "Value: {"
SHOW "Value: {Name"
```

An interpolation expression that is syntactically valid but evaluates to an invalid operation produces the corresponding `ISSUE`.

---

# 30. Text Operations

The collection/text commands operate according to the following rules:

| Command | `TEX` behavior |
|---|---|
| `COUNT` | Number of characters |
| `SEEK` | Find substring |
| `HAS` | Test for substring |
| `BIND` | Produce joined text from a collection |
| `SEVER` | Convert text to collection |
| `CUT` | Trim surrounding whitespace |
| `RAISE` | Uppercase |
| `LOWER` | Lowercase |

---

# 31. Files — FILE

`FILE` represents an open filesystem resource.

Example:

```asm
VAR FILE File EAQ UNSEAL("data.txt", "r")
```

Supported modes:

```text
"r"  read
"w"  write
"a"  append
"rw" read/write
```

---

# 32. UNSEAL

`UNSEAL` opens a filesystem object.

```asm
UNSEAL("data.txt", "r")
```

The result is a `FILE`.

Example:

```asm
VAR FILE File EAQ UNSEAL("data.txt", "r")
```

Failure to open the target produces an `ISSUE`.

---

# 33. SEAL

`SEAL` closes an open file.

```asm
SEAL File
```

Using a sealed or invalid file produces an `ISSUE`.

---

# 34. DRAW

`DRAW` reads data from a file.

```asm
DRAW File
```

The result is text.

A count may optionally be supplied:

```asm
DRAW File, 100
```

which reads up to the requested number of units.

---

# 35. PUT

`PUT` writes data to a file.

```asm
PUT File, "Hello"
```

A write against a read-only file produces an `ISSUE`.

---

# 36. MOVE

`MOVE` changes the current file position.

```asm
MOVE File, 0
```

The default origin is the beginning of the file.

An invalid position produces an `ISSUE`.

---

# 37. File System Commands

## A. HAS

Tests whether a filesystem path exists.

```asm
HAS("data.txt")
```

returns `YES` or `NO`.

## B. MAKE

Creates a filesystem object.

```asm
MAKE "data.txt"
```

## C. ERASE

Deletes a filesystem object.

```asm
ERASE "data.txt"
```

## D. RECALL

Renames a filesystem object.

```asm
RECALL "old.txt", "new.txt"
```

## E. CLONE

Copies a filesystem object.

```asm
CLONE "source.txt", "copy.txt"
```

## F. DELIVER

Moves a filesystem object.

```asm
DELIVER "source.txt", "folder/source.txt"
```

Filesystem operations that target missing or invalid objects produce an `ISSUE`.

---

# 38. Resource Cleanup

File resources should be explicitly sealed.

Example:

```asm
VAR FILE File EAQ UNSEAL("notes.txt", "w")

PUT File, "Virtual Order"
PUT File, "The programmer gives the order."

SEAL File
```

When resource lifetime reaches its end, implementations may apply automatic resource cleanup according to runtime rules.

Explicit commands remain authoritative.

---

# 39. Concurrency

Virtual Order v1.4 introduces concurrent execution.

Concurrency is represented through `TASK` handles.

A task may execute independently of the current flow.

---

# 40. SPAWN

`SPAWN` starts a `JOB` concurrently.

```asm
VAR TASK Worker EAQ SPAWN work()
```

Arguments may be supplied:

```asm
VAR TASK Worker EAQ SPAWN work(10)
```

The result is a `TASK`.

---

# 41. HOLD

`HOLD` waits.

## A. Task Form

```asm
HOLD Worker
```

waits until the referenced task completes.

## B. Timed Form

```asm
HOLD 1000
```

waits for `1000` milliseconds.

---

# 42. CLAIM

`CLAIM` waits for a task and retrieves its result.

```asm
VAR NUM Result EAQ CLAIM Worker
```

If the task produces no value, the result is `EMP`.

---

# 43. HALT

`HALT` requests termination of a task.

```asm
HALT Worker
```

Task termination is cooperative.

---

# 44. Locks

A `LOCK` provides exclusive access to a protected resource.

Example:

```asm
VAR LOCK Guard EAQ LOCK()
```

The runtime creates a synchronization object.

---

# 45. SEIZE

`SEIZE` acquires a lock.

```asm
SEIZE Guard
```

If another task owns the lock, the current task waits.

---

# 46. RELEASE

`RELEASE` releases a previously acquired lock.

```asm
RELEASE Guard
```

Only the owning task may release the lock.

---

# 47. ALIGN

`ALIGN` provides synchronization between concurrent tasks.

```asm
ALIGN Barrier
```

Tasks reaching the same synchronization point wait until the required participants arrive.

---

# 48. Events

Virtual Order uses `WHEN` for event-driven execution.

Basic form:

```asm
WHEN condition
    statements
ENDWHEN
```

Example:

```asm
WHEN Balance > 1000
    SHOW "Balance is high"
ENDWHEN
```

Events remain registered until they are disabled or destroyed.

---

# 49. CHANGED Events

A VMA may be watched for value changes.

```asm
WHEN A1 CHANGED
    SHOW "A1 changed"
ENDWHEN
```

`CHANGED` fires iff:

```text
new_value != old_value
```

Example:

```asm
A1 = EMP
A1 = EMP
```

does not fire.

But:

```asm
A1 = 10
```

fires because:

```text
EMP != 10
```

---

# 50. OLD_VALUE and NEW_VALUE

Inside a `CHANGED` handler, the runtime provides:

```text
OLD_VALUE
NEW_VALUE
```

These are read-only implicit values.

Example:

```asm
WHEN Balance CHANGED
    SHOW OLD_VALUE
    SHOW NEW_VALUE
ENDWHEN
```

They are only valid within the active `CHANGED` handler.

---

# 51. Conditional Event Semantics

Conditional `WHEN` handlers are edge-triggered.

Example:

```asm
WHEN A1 > 100
    SHOW "Threshold reached"
ENDWHEN
```

Each handler maintains an internal `last_state`.

When registered:

```text
last_state = NO
```

Registration itself does not trigger the event.

Whenever a referenced VMA changes:

1. Evaluate the event condition.
2. Store the result as `current_state`.
3. If `last_state == NO` and `current_state == YES`, queue the handler.
4. If `last_state == YES` and `current_state == NO`, re-arm the handler.
5. Otherwise do nothing.
6. Set `last_state = current_state`.

Therefore a condition that remains `YES` does not repeatedly fire on every assignment.

---

# 52. Simultaneous Event Triggers

If multiple events become triggered by the same operation, they are queued in source declaration order unless explicit event priority has been assigned.

---

# 53. Event Queue

Event execution is queued rather than recursively executed.

The event queue is FIFO.

A handler that causes another event does not immediately recursively execute that event. The new event is placed at the back of the queue.

Default maximum queue depth:

```text
1000
```

Exceeding the queue limit produces an `ISSUE`.

---

# 54. ARM

`ARM` enables a disabled event.

```asm
ARM Event
```

An armed event is eligible to trigger.

---

# 55. DISARM

`DISARM` disables an event without destroying it.

```asm
DISARM Event
```

A disarmed event does not respond to normal triggers.

---

# 56. FIRE

`FIRE` manually triggers an event.

```asm
FIRE Event
```

The event is placed into the event queue.

`FIRE` bypasses the normal state-change trigger condition.

---

# 57. RANK

`RANK` assigns an event priority.

```asm
RANK Event, 10
```

Higher-ranked events are dispatched before lower-ranked queued events.

Events with identical rank preserve declaration order.

---

# 58. KILL

`KILL` permanently removes an event.

```asm
KILL Event
```

A killed event cannot trigger again.

---

# 59. SCREEN

`SCREEN` adds an additional condition to an event.

```asm
SCREEN Event, condition
```

The event may fire only if its original trigger and screen condition are both satisfied.

---

# 60. LINK

`LINK` connects two events.

```asm
LINK EventA, EventB
```

When `EventA` fires, `EventB` is queued.

The linked event follows normal event queue semantics.

---

# 61. Modules — PEICE

A `PEICE` defines a self-contained module.

```asm
PEICE mathlib

JOB ADD NUM A NUM B
    GIVE A + B
ENDJOB

ENDPEICE
```

The PEICE normally resides in a `.vo` file.

Example:

```text
mathlib.vo
```

The PEICE name must match the module's declared name.

---

# 62. Bringing Modules — BRING

A module is loaded using:

```asm
BRING mathlib.vo
```

The module becomes available through its namespace.

`BRING` does not merge all module members into the current namespace.

---

# 63. Module Namespaces

Each PEICE receives an independent namespace.

Example:

```asm
BRING mathlib.vo
BRING stringlib.vo
```

Members are accessed through:

```asm
mathlib.ADD(10, 5)
stringlib.SEVER("A,B,C", ",")
```

This prevents name collisions.

---

# 64. Shipping Members — SHIP

Only explicitly shipped members are externally accessible.

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

# 65. Module VMA Isolation

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

but may not directly access:

```asm
bank.Balance
```

unless a future explicit VMA-export feature is introduced.

---

# 66. Module Initialization

A PEICE is initialized the first time it is brought into the program.

Repeated `BRING` operations refer to the already-loaded PEICE.

A PEICE is initialized at most once per program execution.

---

# 67. Module Resolution

Module resolution searches:

1. The directory containing the current source file
2. The project module directory
3. The standard module/library directory

Example:

```asm
BRING mathlib.vo
BRING libs/mathlib.vo
BRING game/entities.vo
```

---

# 68. Duplicate Module Loading

A PEICE may only be loaded once during a program execution.

Repeated:

```asm
BRING mathlib.vo
```

does not create a second module instance.

---

# 69. Circular Module Dependencies

Circular dependencies are illegal.

Example:

```text
a.vo → b.vo → a.vo
```

produces an `ISSUE`.

Example message:

```text
Circular module dependency: a -> b -> a
```

Modules must not be partially initialized to resolve circular dependencies.

---

# 70. Complete Module Example

### `mathlib.vo`

```asm
PEICE mathlib

SHIP ADD
SHIP SUB
SHIP SQUARE

HARD TEX VERSION = "1.4"

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

SHOW mathlib.ADD(10, 5)
SHOW mathlib.SUB(10, 5)
SHOW mathlib.SQUARE(6)
```

Expected output:

```text
15
5
36
```

---

# 71. Complete File Example

```asm
VAR FILE File EAQ UNSEAL("notes.txt", "w")

PUT File, "Virtual Order"
PUT File, "The programmer gives the order."

SEAL File

VAR FILE ReadFile EAQ UNSEAL("notes.txt", "r")

VAR TEX Content EAQ DRAW(ReadFile)

SEAL ReadFile

SHOW Content
```

---

# 72. Complete Collection Example

```asm
VAR COLL Names EAQ ["Alex", "Sam"]

ATTACH Names, "Jordan"
PLACE Names, 1, "Taylor"

SHOW Names
SHOW COUNT(Names)

IF HAS(Names, "Alex")
    SHOW "Alex exists"
ENDIF

VAR NUM Position EAQ SEEK(Names, "Jordan")

SHOW Position
```

---

# 73. Complete String Example

```asm
VAR TEX Name EAQ "Alex"
VAR NUM Score EAQ 95

SHOW "Player: {Name}"
SHOW "Score: {Score}"
SHOW "Next score: {Score + 5}"

VAR TEX Text EAQ "  Hello World  "

Text = CUT(Text)

SHOW RAISE(Text)
SHOW LOWER(Text)

VAR COLL Words EAQ SEVER(Text, " ")

SHOW Words
SHOW COUNT(Text)
```

---

# 74. Complete Concurrency Example

```asm
JOB Worker NUM X
    HOLD 1000
    GIVE X * 2
ENDJOB

VAR TASK TaskA EAQ SPAWN Worker(21)

SHOW "Worker started"

VAR NUM Result EAQ CLAIM TaskA

SHOW "Result:", Result
```

Expected result:

```text
42
```

---

# 75. Complete Event Example

```asm
VAR NUM Counter EAQ 0

WHEN Counter CHANGED
    SHOW "Changed:", OLD_VALUE, "->", NEW_VALUE
ENDWHEN

WHEN Counter >= 3
    SHOW "Threshold reached"
ENDWHEN

Counter = 1
Counter = 2
Counter = 3
Counter = 4
```

The `CHANGED` handler fires on each actual value change.

The `Counter >= 3` handler fires when the condition changes from `NO` to `YES`.

Changing `3` to `4` does not cause another trigger because the condition remains `YES`.

---

# 76. Complete Protected Execution Example

```asm
DO
    VAR NUM Age EAQ TAKE("Age: ")

    DEMAND Age >= 0 "Age cannot be negative"

    SHOW "Valid age:", Age

GRABE
    SHOW "Invalid age"

ENDDO
```

---

# 77. Complete VMA Example

```asm
VAR NUM X EAQ 50

SHOW X

STORE 100 X

SHOW LOAD A1

X = EMP

SHOW X

CLEAN X
```

Conceptually:

```text
X → A1

A1 = 50

STORE 100 A1

A1 = 100

A1 = EMP

CLEAN A1

A1 = unallocated
```

---

# 78. Scope

Virtual Order uses a deliberately simple scope model.

`IF`, `ORIF`, `IFNOT`, `WHILE`, and `FOR` do not create variable scopes.

`JOB`s create their own execution context.

Module state belongs to its PEICE.

Event handlers execute within the event system context.

---

# 79. Type and Operation Rules

Operations must be valid for the participating types.

Invalid examples include:

```asm
EMP + 5
```

or:

```asm
A1[999999]
```

when `A1` is not an indexable value or the index is invalid.

Such operations produce an `ISSUE`.

---

# 80. Runtime Error Classes

The runtime must detect at minimum:

- access to an unallocated VMA;
- invalid VMA;
- double cleanup;
- invalid immutable assignment;
- division by zero;
- invalid collection index;
- invalid slice;
- invalid text index;
- invalid interpolation;
- invalid file handle;
- invalid file mode;
- invalid file operation;
- invalid task handle;
- invalid lock ownership;
- event queue overflow;
- invalid module member access;
- missing module;
- circular module dependency;
- type mismatch;
- illegal `GIVE`;
- illegal `SERVE`;
- illegal resource operation.

---

# 81. Reserved Vocabulary

The following are official Virtual Order keywords:

```text
VAR
EAQ

NUM
DEC
YN
TEX
COLL
EMP
FILE
TASK
LOCK
EVENT

YES
NO

SHOW
TAKE

CLEAN
CLEANALL
AUTOCLEAN

STORE
LOAD

IF
ORIF
IFNOT
ENDIF

WHILE
ENDWHILE

FOR
ENDFOR

JOB
ENDJOB
GIVE

HARD

DEMAND
ISSUE
SERVE
DO
GRABE
ENDDO

PEICE
ENDPEICE
BRING
SHIP

ATTACH
PLACE
ERASE
COUNT
SEEK
HAS
BIND
SEVER
CUT
RAISE
LOWER

UNSEAL
SEAL
DRAW
PUT
MOVE
MAKE
RECALL
CLONE
DELIVER

SPAWN
HOLD
CLAIM
HALT
SEIZE
RELEASE
ALIGN

WHEN
ENDWHEN
CHANGED
OLD_VALUE
NEW_VALUE
ARM
DISARM
FIRE
RANK
KILL
SCREEN
LINK
```

---

# 82. Released Conventional Keywords

Virtual Order does not reserve ordinary programming words simply because they exist in other languages.

The following conventional terms are not part of VO's syntax:

```text
OPEN
CLOSE
READ
WRITE
DELETE
RENAME
COPY
TRANSFER
RETURN
CONST
MODULE
FUNC
EXPORT
ASSERT
ERROR
THROW
TRY
CATCH
```

Their official VO equivalents are:

| Conventional | Virtual Order |
|---|---|
| `OPEN` | `UNSEAL` |
| `CLOSE` | `SEAL` |
| `READ` | `DRAW` |
| `WRITE` | `PUT` |
| `DELETE` | `ERASE` |
| `RENAME` | `RECALL` |
| `COPY` | `CLONE` |
| `TRANSFER` | `DELIVER` |
| `RETURN` | `GIVE` |
| `CONST` | `HARD` |
| `MODULE` | `PEICE` |
| `FUNC` | `JOB` |
| `EXPORT` | `SHIP` |
| `ASSERT` | `DEMAND` |
| `ERROR` | `ISSUE` |
| `THROW` | `SERVE` |
| `TRY` | `DO` |
| `CATCH` | `GRABE` |

These conventional words are not aliases.

---

# 83. VMA Lifetime Reference

| Operation | Effect |
|---|---|
| `VAR NUM X EAQ 10` | Allocates and binds a VMA |
| `VAR EMP X EAQ EMP` | Allocates a VMA containing `EMP` |
| `CLEAN A1` | Frees `A1` |
| `CLEAN X` | Frees the VMA belonging to `X` |
| `AUTOCLEAN ON` | Enables automatic cleanup |
| `AUTOCLEAN OFF` | Disables automatic cleanup |
| `CLEANALL` | Frees all VMAs |
| `X = 20` | Updates X's VMA |
| `X = EMP` | Keeps X's VMA allocated and stores `EMP` |
| `STORE 20 A1` | Stores `20` in `A1` |
| `LOAD A1` | Loads the value of `A1` |

---

# 84. Command Reference

| Command | Purpose |
|---|---|
| `VAR` | Declare a value |
| `EAQ` | Establish and allocate |
| `SHOW` | Display a value |
| `TAKE` | Obtain user input |
| `STORE` | Store directly into a VMA |
| `LOAD` | Load from a VMA |
| `CLEAN` | Free a VMA |
| `CLEANALL` | Free all VMAs |
| `AUTOCLEAN` | Control automatic cleanup |
| `JOB` | Define reusable code |
| `GIVE` | Return a value |
| `HARD` | Define immutable state |
| `DEMAND` | Require a condition |
| `SERVE` | Raise an issue |
| `DO` | Begin protected execution |
| `GRABE` | Handle an issue |
| `PEICE` | Define a module |
| `BRING` | Load a module |
| `SHIP` | Export a module member |
| `ATTACH` | Append to a collection |
| `PLACE` | Insert into a collection |
| `ERASE` | Remove/delete |
| `COUNT` | Count values |
| `SEEK` | Search |
| `HAS` | Test containment/existence |
| `BIND` | Join values into text |
| `SEVER` | Split text |
| `CUT` | Trim text |
| `RAISE` | Uppercase text |
| `LOWER` | Lowercase text |
| `UNSEAL` | Open a file |
| `SEAL` | Close a file |
| `DRAW` | Read from a file |
| `PUT` | Write to a file |
| `MOVE` | Move a file cursor |
| `MAKE` | Create a filesystem object |
| `RECALL` | Rename |
| `CLONE` | Copy |
| `DELIVER` | Move a filesystem object |
| `SPAWN` | Start a concurrent task |
| `HOLD` | Wait |
| `CLAIM` | Wait for and retrieve a task result |
| `HALT` | Request task termination |
| `SEIZE` | Acquire a lock |
| `RELEASE` | Release a lock |
| `ALIGN` | Synchronize tasks |
| `WHEN` | Register an event |
| `ARM` | Enable an event |
| `DISARM` | Disable an event |
| `FIRE` | Manually trigger an event |
| `RANK` | Set event priority |
| `KILL` | Destroy an event |
| `SCREEN` | Add an event filter |
| `LINK` | Link events |

---

# 85. Language Architecture

The conceptual Virtual Order execution pipeline is:

```text
                VO Source
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
            Semantic Analyzer
                   │
        ┌──────────┼──────────┐
        ▼          ▼          ▼
     Type       Module      Symbol
    Checking   Resolution  Resolution
        │          │          │
        └──────────┼──────────┘
                   ▼
               VMA Table
                   │
        ┌──────────┼──────────┐
        ▼          ▼          ▼
      Tasks      Files       Events
        │          │          │
        └──────────┼──────────┘
                   ▼
                Runtime
                   │
                   ▼
                 Output
```

---

# 86. Interpreter Requirements

A conforming interpreter must implement:

- lexical analysis;
- parsing;
- semantic validation;
- VMA allocation;
- VMA cleanup;
- typed values;
- `EMP`;
- expressions;
- control flow;
- `JOB`s;
- `GIVE`;
- modules;
- `BRING`;
- `SHIP`;
- collections;
- text operations;
- TEX interpolation;
- file operations;
- concurrency;
- synchronization;
- events;
- issue handling.

---

# 87. Native Compiler

The reference compiler is:

```text
voc
```

The intended compilation pipeline is:

```text
program.vo
    ↓
Lexer
    ↓
Parser
    ↓
AST
    ↓
Semantic Analysis
    ↓
Native Code Generation
    ↓
Native Backend
    ↓
Executable
```

The compiler must preserve the observable semantics defined by this specification.

---

# 88. Interpreter / Compiler Compatibility

A program accepted by both the interpreter and compiler must behave equivalently.

The implementation mechanism may differ.

For example, a native compiler may represent:

```asm
VAR NUM Age EAQ 18
```

using host-native structures rather than a literal host memory address.

This does not change the required VO VMA semantics.

---

# 89. Event / VMA Interaction

VMA assignments are the primary mechanism by which value-change events are detected.

Example:

```asm
VAR NUM Counter EAQ 0

WHEN Counter CHANGED
    SHOW OLD_VALUE, NEW_VALUE
ENDWHEN

Counter = 1
```

The assignment modifies the VMA and gives the event system an opportunity to evaluate registered handlers.

Operations that modify a VMA indirectly must produce equivalent event behavior.

For example:

```asm
Counter += 1
```

is observable as the same underlying VMA value change as:

```asm
Counter = Counter + 1
```

---

# 90. Event Re-entrancy

Events do not recursively interrupt the currently executing event handler.

Example:

```asm
WHEN A1 CHANGED
    A2 = 10
ENDWHEN

WHEN A2 CHANGED
    SHOW "A2 changed"
ENDWHEN
```

Changing `A1` first queues the `A1` event.

While it executes, modifying `A2` queues the `A2` event.

The `A2` event executes after the current handler finishes.

This preserves FIFO event dispatch.

---

# 91. Resource Handles

`FILE`, `TASK`, `LOCK`, and `EVENT` values represent runtime-managed resources.

An invalid resource handle is not equivalent to an ordinary `EMP` value unless explicitly specified by the relevant operation.

Operations on invalid handles produce an `ISSUE`.

---

# 92. Module and Runtime Isolation

Each module owns its own:

- identifiers;
- private VMAs;
- private module state;
- private members.

Public behavior is exposed through shipped members.

Module namespace access does not imply direct VMA ownership by the caller.

---

# 93. Example: Full v1.4 Program

```asm
; ==========================================
; Virtual Order v1.4 Example
; ==========================================

BRING mathlib.vo

VAR TEX Name EAQ TAKE("Name: ")
VAR NUM Age EAQ TAKE("Age: ")

VAR COLL Scores EAQ [10, 20, 30]

ATTACH Scores, 40
PLACE Scores, 1, 15

SHOW "Name: {Name}"
SHOW "Age: {Age}"
SHOW "Scores:", Scores

IF HAS(Scores, 30)
    SHOW "Score found"
ENDIF

WHEN Age CHANGED
    SHOW "Age changed from {OLD_VALUE} to {NEW_VALUE}"
ENDWHEN

IF Age >= 18
    SHOW "Adult, {Name}"
IFNOT
    SHOW "Minor, {Name}"
ENDIF

JOB Work NUM X
    GIVE X * 2
ENDJOB

VAR TASK Worker EAQ SPAWN Work(21)

VAR NUM Result EAQ CLAIM Worker

SHOW "Task result: {Result}"

DO
    DEMAND Age >= 0 "Age cannot be negative"

GRABE
    SHOW "Invalid age"

ENDDO
```

---

# 94. v1.4 Feature Summary

## Core

```text
NUM
DEC
YN
TEX
COLL
EMP
```

## Memory

```text
VMA
VAR
EAQ
STORE
LOAD
CLEAN
CLEANALL
AUTOCLEAN
```

## Input / Output

```text
SHOW
TAKE
```

## Control Flow

```text
IF
ORIF
IFNOT
WHILE
FOR
```

## Jobs

```text
JOB
GIVE
```

## Immutability

```text
HARD
```

## Issues

```text
DEMAND
ISSUE
SERVE
DO
GRABE
```

## Modules

```text
PEICE
BRING
SHIP
```

## Collections / Strings

```text
ATTACH
PLACE
ERASE
COUNT
SEEK
HAS
BIND
SEVER
CUT
RAISE
LOWER
```

## TEX

```text
Interpolation: {expression}
Escaped left brace: {{
Escaped right brace: }}
```

## Files

```text
FILE
UNSEAL
SEAL
DRAW
PUT
MOVE
MAKE
RECALL
CLONE
DELIVER
```

## Concurrency

```text
TASK
SPAWN
HOLD
CLAIM
HALT
LOCK
SEIZE
RELEASE
ALIGN
```

## Events

```text
EVENT
WHEN
CHANGED
OLD_VALUE
NEW_VALUE
ARM
DISARM
FIRE
RANK
KILL
SCREEN
LINK
```

---

# 95. Changelog

## v1.3 → v1.4

| # | Change | Resolution |
|---|---|---|
| 27 | No standardized collection manipulation | Added `ATTACH`, `PLACE`, `ERASE` |
| 28 | No collection length operation | Added `COUNT` |
| 29 | No collection/text search operation | Added `SEEK` |
| 30 | No containment operation | Added `HAS` |
| 31 | No collection-to-text operation | Added `BIND` |
| 32 | No text splitting operation | Added `SEVER` |
| 33 | No text trimming | Added `CUT` |
| 34 | No text case conversion | Added `RAISE` and `LOWER` |
| 35 | No collection indexing specification | Added zero-based indexing |
| 36 | No collection slicing specification | Added `[start:end]` slicing |
| 37 | No text indexing specification | Added zero-based text indexing |
| 38 | No string interpolation | Added TEX interpolation using `{expression}` |
| 39 | No literal-brace escape syntax | Added `{{` and `}}` |
| 40 | No filesystem resource type | Added `FILE` |
| 41 | No file opening command | Added `UNSEAL` |
| 42 | No file closing command | Added `SEAL` |
| 43 | No file reading command | Added `DRAW` |
| 44 | No file writing command | Added `PUT` |
| 45 | No file seeking command | Added `MOVE` |
| 46 | No filesystem creation command | Added `MAKE` |
| 47 | No filesystem rename command | Added `RECALL` |
| 48 | No filesystem copy command | Added `CLONE` |
| 49 | No filesystem transfer command | Added `DELIVER` |
| 50 | No concurrency model | Added `TASK` and task execution |
| 51 | No concurrent execution command | Added `SPAWN` |
| 52 | No task waiting model | Added `HOLD` |
| 53 | No task result retrieval | Added `CLAIM` |
| 54 | No task cancellation | Added `HALT` |
| 55 | No synchronization lock model | Added `LOCK`, `SEIZE`, `RELEASE` |
| 56 | No synchronization barrier | Added `ALIGN` |
| 57 | Event control was limited | Added advanced event control |
| 58 | No event enable/disable | Added `ARM` / `DISARM` |
| 59 | No manual event triggering | Added `FIRE` |
| 60 | No event prioritization | Added `RANK` |
| 61 | No event destruction | Added `KILL` |
| 62 | No event filtering | Added `SCREEN` |
| 63 | No event chaining | Added `LINK` |
| 64 | Comment syntax needed formalization | Standardized `;` and `;;...;;` |
| 65 | Generic programming words were unnecessarily treated as language concepts | Released conventional terms from the VO vocabulary where VO has official replacements |

---

# 96. Official Virtual Order Philosophy

Virtual Order is designed around direct, command-oriented interaction with a virtual memory system.

The programmer gives an order.

The runtime executes that order.

The VMA holds the result.

The language therefore prefers terminology such as:

```text
GIVE
SHIP
BRING
SERVE
DEMAND
SEIZE
RELEASE
UNSEAL
SEAL
DRAW
PUT
CLONE
DELIVER
ERASE
```

rather than simply reproducing terminology from conventional programming languages.

TEX interpolation extends this philosophy into output without introducing a separate formatting language:

```asm
SHOW "Balance: {Balance}"
```

The central model remains:

```text
Identifier
    ↓
Virtual Memory Address
    ↓
Value
```

The defining principle of Virtual Order is:

> **THE PROGRAMMER GIVES THE ORDER.**
>
> **THE VIRTUAL MEMORY ADDRESSES OBEY.**
>
> **THE RUNTIME EXECUTES.**