#ifndef VO_RT_H
#define VO_RT_H
/* =========================================================================
 * vo_rt: the runtime SUPPORT LIBRARY linked into every natively-compiled
 * Virtual Order program - the equivalent of libc for VO.
 *
 * IMPORTANT DESIGN BOUNDARY: nothing in here ever takes an ASTNode or
 * decides program behavior based on source-level constructs. Every
 * function here operates purely on already-computed VoValue data (or
 * plain C scalars/strings). All control flow (IF/WHILE/FOR/GOTO), all
 * statement sequencing, and all decisions about *which* of these
 * functions to call and in what order are baked into real x86-64
 * machine code at compile time by codegen.c. That's what makes this a
 * compiler and not an interpreter with extra steps: this library plays
 * the same role printf/malloc/memcpy play for a C program.
 * ========================================================================= */
#include <stddef.h>
#include <stdio.h>

typedef enum { VO_NUM, VO_DEC, VO_TEX, VO_YN, VO_COLL, VO_NULL, VO_EMP, VO_FILE, VO_TASK, VO_LOCK } VoType;

typedef struct VoValue {
    VoType type;
    long   num;             /* VO_NUM  */
    double dec;             /* VO_DEC  */
    char  *tex;             /* VO_TEX - heap owned */
    int    yn;              /* VO_YN   */
    struct VoValue *items;  /* VO_COLL - heap owned array */
    int    count, capacity; /* VO_COLL */

    /* VO_FILE */
    FILE  *file;            /* stdio handle (owned only by SEAL) */
    char  *file_mode;       /* "r"/"w"/"a"/"r+" - heap owned */

    /* VO_TASK (SPAWN result; ran once by HOLD/CLAIM) */
    char  *job_name;        /* display name, heap owned */
    int    job_index;       /* index into vo_job_tab (emitted by codegen) */
    struct VoValue *task_args;   /* deep-copied call arguments */
    int    task_argc;
    int    task_state;      /* 0 pending, 1 done, 2 cancelled */
    struct VoValue *task_result; /* VO_TASK: result after run, heap owned */

    /* VO_LOCK (SEIZE/RELEASE barrier token) */
    int    lock_held;
} VoValue;

/* ---- compiled-job table (emitted as .data by codegen, consumed by the
   TASK machinery below). field ORDER and SIZE must match codegen's
   emission exactly: name*, argc, fn*, params[16]*, ret*. ---- */
typedef struct VoJobInfo {
    const char *name;
    int         argc;
    void      (*fn)(void);
    VoValue   *params[16];
    VoValue   *ret;
} VoJobInfo;
extern VoJobInfo vo_job_tab[];
extern int       vo_job_tab_count;

/* ---- construction / lifetime ---- */
void vo_set_num(VoValue *v, long n);
void vo_set_dec(VoValue *v, double d);
void vo_set_tex(VoValue *v, const char *s);
void vo_set_yn(VoValue *v, int b);
void vo_set_null(VoValue *v);
void vo_set_emp(VoValue *v);
void vo_set_dec_from_ptr(VoValue *v, const double *d); /* codegen passes double constants by address */
void vo_coll_new(VoValue *v);
void vo_copy(VoValue *dst, const VoValue *src);   /* deep copy, dst overwritten (not freed first) */
void vo_free(VoValue *v);                          /* frees owned heap data, sets *v = NULL value */
void vo_assign(VoValue *dst, VoValue *src);        /* frees dst's old contents, then deep-copies src in */

/* ---- arithmetic / bitwise: dst may alias a and/or b ---- */
void vo_add(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_sub(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_mul(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_div(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_mod(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_pow(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_shl(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_shr(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_band(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_bxor(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_bor(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_neg(VoValue *dst, VoValue *a, int line);

/* ---- comparisons -> VO_YN ---- */
void vo_eq(VoValue *dst, VoValue *a, VoValue *b);
void vo_neq(VoValue *dst, VoValue *a, VoValue *b);
void vo_lt(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_gt(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_le(VoValue *dst, VoValue *a, VoValue *b, int line);
void vo_ge(VoValue *dst, VoValue *a, VoValue *b, int line);

/* ---- logical ---- */
void vo_land(VoValue *dst, VoValue *a, VoValue *b);
void vo_lor(VoValue *dst, VoValue *a, VoValue *b);
void vo_lxor(VoValue *dst, VoValue *a, VoValue *b);
void vo_lnot(VoValue *dst, VoValue *a);
int  vo_truthy(VoValue *v);

/* ---- collections / length ---- */
void vo_length(VoValue *dst, VoValue *a, int line);
void vo_coll_push(VoValue *coll, VoValue *item, int line);
void vo_index_get(VoValue *dst, VoValue *coll, VoValue *idx, int line);
void vo_index_set(VoValue *coll, VoValue *idx, VoValue *val, int line);

/* ---- I/O ---- */
void vo_show(VoValue *v);
void vo_show_part(VoValue *v);   /* prints "v " (multi-operand SHOW, non-final parts) */

/* ---- slicing / TEX interpolation ---- */
void vo_slice(VoValue *out, VoValue *arr, VoValue *start, VoValue *end, int line);
void vo_tex_interp_append_cstr(VoValue *acc, const char *s);
void vo_tex_interp_append_val(VoValue *acc, VoValue *v);

/* ---- builtin commands (v1.4). `out`/`coll`/`task` are VoValue slots;
   mutating commands operate in place on the slot passed in (codegen passes
   a variable's own storage, mirroring the tree-walking runtime's lvalues). ---- */
void vo_cmd_attach(VoValue *coll, VoValue *item, int line);
void vo_cmd_place(VoValue *coll, VoValue *idx, VoValue *item, int line);
void vo_cmd_erase_coll(VoValue *coll, VoValue *idx, int line);
void vo_cmd_erase_file(VoValue *path, int line);
void vo_cmd_count(VoValue *out, VoValue *v, int line);
void vo_cmd_take(VoValue *out, VoValue *prompt, int line);
void vo_cmd_seek(VoValue *out, VoValue *hay, VoValue *ndl, int line);
void vo_cmd_has_path(VoValue *out, VoValue *p, int line);
void vo_cmd_has(VoValue *out, VoValue *hay, VoValue *ndl, int line);
void vo_cmd_bind(VoValue *out, VoValue *coll, VoValue *sep, int line);
void vo_cmd_sever(VoValue *out, VoValue *tex, VoValue *sep, int line);
void vo_cmd_cut(VoValue *out, VoValue *tex, int line);
void vo_cmd_case(VoValue *out, VoValue *tex, int upper, int line);
void vo_cmd_unseal(VoValue *out, VoValue *path, VoValue *mode, int has_mode, int line);
void vo_cmd_seal(VoValue *f, int line);
void vo_cmd_draw(VoValue *out, VoValue *f, VoValue *count, int has_count, int line);
void vo_cmd_put(VoValue *f, VoValue *t, int line);
void vo_cmd_move(VoValue *f, VoValue *pos, int line);
void vo_cmd_make(VoValue *p1, int line);
void vo_cmd_recall(VoValue *p1, VoValue *p2, int line);
void vo_cmd_clone(VoValue *p1, VoValue *p2, int line);
void vo_cmd_deliver(VoValue *p1, VoValue *p2, int line);

/* tasks / concurrency (single-threaded deferred, same as the interpreter) */
void vo_cmd_spawn(VoValue *out, int job_index, int argc, int line);
void vo_task_set_arg(VoValue *task, int i, VoValue *arg, int line);
void vo_cmd_hold(VoValue *op, int line);          /* task OR numeric milliseconds */
void vo_cmd_claim(VoValue *out, VoValue *task, int line);
void vo_cmd_halt(VoValue *task, int line);
void vo_task_run(VoValue *task, int line);
void vo_sleep_ms(long ms);

/* locks/barriers: SEIZE/RELEASE/ALIGN (no source-level constructor; parity
   with the interpreter's value semantics) */
void vo_cmd_seize(VoValue *lock, int line);
void vo_cmd_release(VoValue *lock, int line);
void vo_cmd_align(VoValue *barrier, int line);

/* ---- assign-op helper (mirrors spec: COLL += appends) ---- */
void vo_plus_assign(VoValue *dst, VoValue *current, VoValue *rhs, int line);

/* ---- v1.3 control-flow helpers ---- */
/* vo_demand: if cond is not truthy (and message, if provided, is
   non-NULL), raise a fatal runtime error. Returns the truthiness so
   codegen can branch (used for DEMAND inside a DO block). */
int  vo_demand_check(VoValue *cond, int line);

/* ---- fatal runtime error: prints "[line N] Runtime error: msg" to
   stderr and exits with status 1, matching the tree-walking runtime ---- */
void vo_error(int line, const char *msg);
void vo_errorf(int line, const char *fmt, ...);

/* ---- program entry helpers ---- */
void vo_rt_init(void);
void vo_rt_shutdown(void);

#endif
