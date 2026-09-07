#include "vo_rt.h"

/* vo_main is the real program, entirely generated x86-64 machine code
   emitted by codegen.c. This stub just sets up stdio buffering and
   hands control to it - it is the only "runtime scaffolding" a
   compiled VO program needs, exactly analogous to a C program's
   crt0/_start handoff to main(). */
void vo_main(void);

int main(void) {
    vo_rt_init();
    vo_main();
    vo_rt_shutdown();
    return 0;
}
