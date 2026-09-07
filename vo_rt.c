#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vo_rt.h"

void vo_error(int line, const char *msg) {
    fprintf(stderr, "[line %d] Runtime error: %s\n", line, msg);
    exit(1);
}

static int is_numeric(VoValue *v) { return v->type == VO_NUM || v->type == VO_DEC; }
static double as_double(VoValue *v) { return v->type == VO_DEC ? v->dec : (double)v->num; }

void vo_set_num(VoValue *v, long n)      { v->type = VO_NUM;  v->num = n; }
void vo_set_dec(VoValue *v, double d)    { v->type = VO_DEC;  v->dec = d; }
void vo_set_tex(VoValue *v, const char *s) { v->type = VO_TEX; v->tex = strdup(s ? s : ""); }
void vo_set_yn(VoValue *v, int b)        { v->type = VO_YN;   v->yn = b ? 1 : 0; }
void vo_set_null(VoValue *v)             { memset(v, 0, sizeof(*v)); v->type = VO_NULL; }
void vo_set_emp(VoValue *v)              { memset(v, 0, sizeof(*v)); v->type = VO_EMP; }
void vo_set_dec_from_ptr(VoValue *v, const double *d) { v->type = VO_DEC; v->dec = *d; }
void vo_coll_new(VoValue *v)             { memset(v, 0, sizeof(*v)); v->type = VO_COLL; }

void vo_free(VoValue *v) {
    if (v->type == VO_TEX && v->tex) free(v->tex);
    if (v->type == VO_COLL && v->items) {
        for (int i = 0; i < v->count; i++) vo_free(&v->items[i]);
        free(v->items);
    }
    vo_set_null(v);
}

void vo_assign(VoValue *dst, VoValue *src) {
    if (dst == src) return;
    vo_free(dst);
    vo_copy(dst, src);
}

void vo_copy(VoValue *dst, const VoValue *src) {
    switch (src->type) {
        case VO_TEX:
            dst->type = VO_TEX; dst->tex = strdup(src->tex ? src->tex : "");
            break;
        case VO_COLL: {
            dst->type = VO_COLL; dst->count = src->count; dst->capacity = src->count;
            dst->items = src->count ? malloc(sizeof(VoValue) * src->count) : NULL;
            for (int i = 0; i < src->count; i++) vo_copy(&dst->items[i], &src->items[i]);
            break;
        }
        default:
            *dst = *src; /* NUM/DEC/YN/NULL are plain scalars */
            break;
    }
}

/* value -> display string, matching the tree-walking runtime's format */
static char *to_cstr(VoValue *v) {
    char buf[64];
    switch (v->type) {
        case VO_NUM: snprintf(buf, sizeof(buf), "%ld", v->num); return strdup(buf);
        case VO_DEC: snprintf(buf, sizeof(buf), "%g", v->dec); return strdup(buf);
        case VO_TEX: return strdup(v->tex ? v->tex : "");
        case VO_YN:  return strdup(v->yn ? "YES" : "NO");
        case VO_NULL: return strdup("NULL");
        case VO_EMP: return strdup("EMP");
        case VO_COLL: {
            size_t cap = 64, len = 1;
            char *out = malloc(cap);
            strcpy(out, "[");
            for (int i = 0; i < v->count; i++) {
                char *piece = to_cstr(&v->items[i]);
                size_t plen = strlen(piece);
                while (len + plen + 4 > cap) { cap *= 2; out = realloc(out, cap); }
                if (i > 0) { strcpy(out + len - 1, ", "); len += 1; out[len] = '\0'; }
                strcpy(out + len - 1, piece);
                len += plen;
                free(piece);
            }
            if (len + 2 > cap) { cap += 2; out = realloc(out, cap); }
            out[len - 1] = ']'; out[len] = '\0';
            return out;
        }
    }
    return strdup("");
}

void vo_show(VoValue *v) {
    char *s = to_cstr(v);
    printf("%s\n", s);
    free(s);
}

int vo_truthy(VoValue *v) {
    switch (v->type) {
        case VO_NUM: return v->num != 0;
        case VO_DEC: return v->dec != 0.0;
        case VO_TEX: return v->tex && v->tex[0] != '\0';
        case VO_YN:  return v->yn != 0;
        case VO_COLL: return v->count != 0;
        case VO_NULL: return 0;
        case VO_EMP: return 0;
    }
    return 0;
}

static int values_equal(VoValue *a, VoValue *b) {
    if (is_numeric(a) && is_numeric(b)) return as_double(a) == as_double(b);
    if (a->type != b->type) return 0;
    switch (a->type) {
        case VO_TEX: return strcmp(a->tex ? a->tex : "", b->tex ? b->tex : "") == 0;
        case VO_YN:  return a->yn == b->yn;
        case VO_NULL: return 1;
        case VO_EMP: return 1;
        case VO_COLL:
            if (a->count != b->count) return 0;
            for (int i = 0; i < a->count; i++) if (!values_equal(&a->items[i], &b->items[i])) return 0;
            return 1;
        default: return 0;
    }
}

/* ---- arithmetic (dst may alias a/b: compute into locals first) ---- */
void vo_add(VoValue *dst, VoValue *a, VoValue *b, int line) {
    if (a->type == VO_TEX || b->type == VO_TEX) {
        char *ls = to_cstr(a), *rs = to_cstr(b);
        char *cat = malloc(strlen(ls) + strlen(rs) + 1);
        strcpy(cat, ls); strcat(cat, rs);
        if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
        dst->type = VO_TEX; dst->tex = cat;
        free(ls); free(rs);
        return;
    }
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'+' needs numeric or text operands");
    int use_dec = (a->type == VO_DEC || b->type == VO_DEC);
    double dv = as_double(a) + as_double(b);
    long   nv = a->num + b->num;
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
    if (use_dec) vo_set_dec(dst, dv); else vo_set_num(dst, nv);
}

#define NUMERIC_BINOP(NAME, OPSYM, EXPR_DEC, EXPR_NUM)                         \
void NAME(VoValue *dst, VoValue *a, VoValue *b, int line) {                    \
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'" OPSYM "' needs numeric operands"); \
    int use_dec = (a->type == VO_DEC || b->type == VO_DEC);                   \
    double da = as_double(a), db = as_double(b);                              \
    long na = a->num, nb = b->num;                                            \
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);            \
    if (use_dec) vo_set_dec(dst, (EXPR_DEC)); else vo_set_num(dst, (EXPR_NUM)); \
}

NUMERIC_BINOP(vo_sub, "-", da - db, na - nb)
NUMERIC_BINOP(vo_mul, "*", da * db, na * nb)

void vo_div(VoValue *dst, VoValue *a, VoValue *b, int line) {
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'/' needs numeric operands");
    if (as_double(b) == 0.0) vo_error(line, "division by zero");
    int use_dec = (a->type == VO_DEC || b->type == VO_DEC);
    double dv = as_double(a) / as_double(b);
    long   nv = a->num / b->num;
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
    if (use_dec) vo_set_dec(dst, dv); else vo_set_num(dst, nv);
}

void vo_mod(VoValue *dst, VoValue *a, VoValue *b, int line) {
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'%' needs numeric operands");
    int use_dec = (a->type == VO_DEC || b->type == VO_DEC);
    if (use_dec) {
        if (as_double(b) == 0.0) vo_error(line, "division by zero");
        double dv = fmod(as_double(a), as_double(b));
        if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
        vo_set_dec(dst, dv);
    } else {
        if (b->num == 0) vo_error(line, "division by zero");
        long nv = a->num % b->num;
        if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
        vo_set_num(dst, nv);
    }
}

void vo_pow(VoValue *dst, VoValue *a, VoValue *b, int line) {
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'**' needs numeric operands");
    double result = pow(as_double(a), as_double(b));
    int int_result = (a->type == VO_NUM && b->type == VO_NUM && b->num >= 0);
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
    if (int_result) vo_set_num(dst, (long)llround(result));
    else vo_set_dec(dst, result);
}

#define INT_BINOP(NAME, OPSYM, EXPR)                                          \
void NAME(VoValue *dst, VoValue *a, VoValue *b, int line) {                    \
    if (a->type != VO_NUM || b->type != VO_NUM) vo_error(line, "'" OPSYM "' needs NUM operands"); \
    long na = a->num, nb = b->num;                                            \
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);            \
    vo_set_num(dst, (EXPR));                                                  \
}

INT_BINOP(vo_shl, "<<", na << nb)
INT_BINOP(vo_shr, ">>", na >> nb)
INT_BINOP(vo_band, "&", na & nb)
INT_BINOP(vo_bxor, "^", na ^ nb)
INT_BINOP(vo_bor, "|", na | nb)

void vo_neg(VoValue *dst, VoValue *a, int line) {
    if (!is_numeric(a)) vo_error(line, "unary '-' needs a numeric operand");
    if (a->type == VO_DEC) { double d = -a->dec; vo_set_dec(dst, d); }
    else { long n = -a->num; vo_set_num(dst, n); }
}

/* ---- comparisons ---- */
void vo_eq(VoValue *dst, VoValue *a, VoValue *b)  { int r = values_equal(a, b);  vo_set_yn(dst, r); }
void vo_neq(VoValue *dst, VoValue *a, VoValue *b) { int r = !values_equal(a, b); vo_set_yn(dst, r); }

#define CMP_OP(NAME, OPSYM, EXPR)                                             \
void NAME(VoValue *dst, VoValue *a, VoValue *b, int line) {                    \
    if (!is_numeric(a) || !is_numeric(b)) vo_error(line, "'" OPSYM "' needs numeric operands"); \
    double da = as_double(a), db = as_double(b);                              \
    vo_set_yn(dst, (EXPR));                                                   \
}
CMP_OP(vo_lt, "<",  da < db)
CMP_OP(vo_gt, ">",  da > db)
CMP_OP(vo_le, "<=", da <= db)
CMP_OP(vo_ge, ">=", da >= db)

/* ---- logical ---- */
void vo_land(VoValue *dst, VoValue *a, VoValue *b) { int r = vo_truthy(a) && vo_truthy(b); vo_set_yn(dst, r); }
void vo_lor(VoValue *dst, VoValue *a, VoValue *b)  { int r = vo_truthy(a) || vo_truthy(b); vo_set_yn(dst, r); }
void vo_lxor(VoValue *dst, VoValue *a, VoValue *b) { int r = vo_truthy(a) ^ vo_truthy(b);  vo_set_yn(dst, r); }
void vo_lnot(VoValue *dst, VoValue *a)             { int r = !vo_truthy(a); vo_set_yn(dst, r); }

/* ---- collections ---- */
void vo_length(VoValue *dst, VoValue *a, int line) {
    long n;
    switch (a->type) {
        case VO_TEX:  n = (long)strlen(a->tex ? a->tex : ""); break;
        case VO_COLL: n = a->count; break;
        default: vo_error(line, "LENGTH needs TEX or COLL"); return;
    }
    vo_set_num(dst, n);
}

void vo_coll_push(VoValue *coll, VoValue *item, int line) {
    if (coll->type != VO_COLL) vo_error(line, "'+=' append needs a COLL target");
    if (coll->count == coll->capacity) {
        coll->capacity = coll->capacity ? coll->capacity * 2 : 4;
        coll->items = realloc(coll->items, sizeof(VoValue) * coll->capacity);
    }
    vo_copy(&coll->items[coll->count++], item);
}

void vo_index_get(VoValue *dst, VoValue *coll, VoValue *idx, int line) {
    if (coll->type != VO_COLL) vo_error(line, "indexing needs a COLL");
    if (idx->type != VO_NUM) vo_error(line, "index must be NUM");
    if (idx->num < 0 || idx->num >= coll->count) vo_error(line, "index out of range");
    if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
    vo_copy(dst, &coll->items[idx->num]);
}

void vo_index_set(VoValue *coll, VoValue *idx, VoValue *val, int line) {
    if (coll->type != VO_COLL) vo_error(line, "indexing needs a COLL");
    if (idx->type != VO_NUM) vo_error(line, "index must be NUM");
    if (idx->num < 0 || idx->num >= coll->count) vo_error(line, "index out of range");
    vo_free(&coll->items[idx->num]);
    vo_copy(&coll->items[idx->num], val);
}

void vo_plus_assign(VoValue *dst, VoValue *current, VoValue *rhs, int line) {
    if (current->type == VO_COLL) {
        VoValue tmp; vo_copy(&tmp, current);
        vo_coll_push(&tmp, rhs, line);
        if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
        *dst = tmp;
        return;
    }
    vo_add(dst, current, rhs, line);
}

int vo_demand_check(VoValue *cond, int line) {
    (void)line;
    return vo_truthy(cond);
}

void vo_rt_init(void) { setvbuf(stdout, NULL, _IOLBF, 0); }
void vo_rt_shutdown(void) { fflush(stdout); }
