#ifndef VO_ANALYZER_H
#define VO_ANALYZER_H

#include "ast.h"

/* ---------------------------------------------------------------------
 * Semantic analysis: symbol table + VMA allocator.
 *
 * Runs after parsing, on an already-syntactically-valid AST. Performs:
 *
 *   - Scope-aware identifier tracking for VAR/CONST decls. Only two
 *     kinds of construct introduce a new scope, per spec Sec 6.D: a
 *     WHEN...ENDWHEN block, and the top-level program. IF/WHILE/FOR
 *     bodies are analyzed in their enclosing scope.
 *   - Undeclared-identifier detection on every read/write of a
 *     NODE_IDENTIFIER.
 *   - Redeclaration detection (same name declared twice in one scope).
 *   - CONST-write detection (assignment/++/-- against a CONST_DECL'd
 *     identifier).
 *   - A VMA table simulating the first-fit allocator from spec Sec
 *     6.A/6.B (numeric-exhausts-before-next-letter ordering, freed
 *     addresses reused ahead of never-used ones) and the aliasing ban
 *     from spec Sec 5 (`VAR ... EAQ <already-allocated VMA>`).
 *   - AUTOCLEAN-scoped freeing: identifiers declared while AUTOCLEAN is
 *     ON are freed when their owning scope (WHEN block / program) is
 *     popped.
 *   - Whole-program GOTO/label resolution (flat namespace, forward
 *     references allowed, matching assembly-style control flow).
 *   - OLD_VALUE / NEW_VALUE restricted to the body of a
 *     `WHEN <vma> CHANGED` handler.
 *
 * Deliberately NOT done here (see the design note in analyzer.c above
 * check_vma_access): liveness/reachability of *raw* VMA addresses
 * (e.g. is `A5` still allocated at this exact program point) is only
 * checked with a flow-insensitive, single-linear-pass approximation,
 * and is reported as a warning rather than an error, since IF/WHILE/
 * FOR branches make the real answer path-dependent and this pass does
 * not build a CFG. Lexical facts (declared/not declared, const/not
 * const, label exists/not) are sound and reported as hard errors.
 *
 * Returns 1 if any hard semantic error was found (diagnostics are
 * printed to stderr as they're discovered, "[line N] ..." style,
 * matching the parser/lexer), 0 if the program is semantically clean.
 * Warnings never affect the return value.
 * ------------------------------------------------------------------- */
int analyze_program(ASTNode *program);

#endif
