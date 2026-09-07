#ifndef VO_CODEGEN_H
#define VO_CODEGEN_H

#include <stdio.h>
#include "ast.h"

typedef enum { CG_TARGET_LINUX, CG_TARGET_WINDOWS } CgTarget;

/* Emits x86-64 GAS (AT&T) assembly for `program` to `out`. The emitted
   assembly assumes it will be assembled+linked together with vo_rt.c
   (compiled for the same target). Returns 0 on success, 1 if the
   program uses a construct this backend doesn't yet lower to native
   code (diagnostics go to stderr). */
int codegen_compile(ASTNode *program, CgTarget target, FILE *out);

#endif
