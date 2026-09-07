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

typedef enum { VO_NUM, VO_DEC, VO_TEX, VO_YN, VO_COLL, VO_NULL, VO_EMP } VoType;

typedef struct VoValue {
    VoType type;
    long   num;             /* VO_NUM  */
    double dec;             /* VO_DEC  */
    char  *tex;             /* VO_TEX - heap owned */
    int    yn;              /* VO_YN   */
    struct VoValue *items;  /* VO_COLL - heap owned array */
    int    count, capacity; /* VO_COLL */
} VoValue;

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

/* ---- program entry helpers ---- */
void vo_rt_init(void);
void vo_rt_shutdown(void);

#endif
