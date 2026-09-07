#ifndef VO_RUNTIME_H
#define VO_RUNTIME_H

#include "ast.h"

/* ---------------------------------------------------------------------
 * Runtime: executes an already-parsed (and ideally already-analyzed)
 * program, implementing the dynamic semantics from spec Sec 6:
 *
 *   - Real VMA storage (values, not just allocation bookkeeping).
 *   - Edge-triggered WHEN handlers: boolean conditions carry a
 *     per-handler `last_state` and fire only on false->true crossing
 *     (Sec 6.E); `WHEN <vma> CHANGED` fires iff new_value != old_value.
 *   - A FIFO event queue: assignments enqueue newly-true handlers
 *     rather than invoking them inline; the currently executing
 *     statement/handler always finishes before the queue is drained;
 *     handlers a handler triggers go to the back of the queue
 *     (breadth-first, never onto the call stack); a max queue depth
 *     guards against infinite trigger loops.
 *   - AUTOCLEAN-scoped freeing: WHEN blocks and the top-level program
 *     are the only scopes: on exit, every identifier declared while
 *     AUTOCLEAN was ON gets its VMA freed.
 *   - GOTO/labels: the whole program is compiled to one flat,
 *     jump-based instruction stream up front specifically so GOTO can
 *     target any label anywhere (forward refs included) with a single
 *     instruction pointer - see the design note above `compile_block`
 *     in runtime.c.
 *
 * Returns a process-style exit code: 0 on a clean run, non-zero if a
 * runtime error (e.g. undeclared-VMA access, event queue overflow)
 * stopped execution early.
 * ------------------------------------------------------------------- */
int run_program(ASTNode *program, const char *source_path);

#endif
