#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "vo_rt.h"

/* vo_job_tab / vo_job_tab_count are DEFINED by the codegen-emitted .s
   (per-program .data), never here - this library only consumes them. */

void vo_error(int line, const char *msg) {
    fprintf(stderr, "[line %d] Runtime error: %s\n", line, msg);
    exit(1);
}

void vo_errorf(int line, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vo_error(line, buf);
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
    if (v->type == VO_FILE && v->file_mode) free(v->file_mode);
    if (v->type == VO_TASK) {
        free(v->job_name);
        for (int i = 0; i < v->task_argc; i++) vo_free(&v->task_args[i]);
        free(v->task_args);
        if (v->task_result) { vo_free(v->task_result); free(v->task_result); }
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
        case VO_FILE:
            dst->type = VO_FILE; dst->file = src->file;
            dst->file_mode = strdup(src->file_mode ? src->file_mode : "");
            break;
        case VO_TASK: {
            dst->type = VO_TASK;
            dst->job_name = strdup(src->job_name ? src->job_name : "");
            dst->job_index = src->job_index;
            dst->task_argc = src->task_argc;
            dst->task_state = src->task_state;
            dst->task_args = src->task_argc ? malloc(sizeof(VoValue) * src->task_argc) : NULL;
            for (int i = 0; i < src->task_argc; i++) vo_copy(&dst->task_args[i], &src->task_args[i]);
            dst->task_result = src->task_result ? malloc(sizeof(VoValue)) : NULL;
            if (dst->task_result) vo_copy(dst->task_result, src->task_result);
            break;
        }
        case VO_LOCK:
            dst->type = VO_LOCK; dst->lock_held = src->lock_held;
            break;
        default:
            *dst = *src; /* NUM/DEC/YN/NULL/EMP are plain scalars */
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
        case VO_FILE: { snprintf(buf, sizeof(buf), "FILE@%p", (void*)v->file); return strdup(buf); }
        case VO_TASK: { snprintf(buf, sizeof(buf), "TASK(%s)", v->job_name ? v->job_name : "?"); return strdup(buf); }
        case VO_LOCK: { snprintf(buf, sizeof(buf), "LOCK#%d", v->lock_held); return strdup(buf); }
        case VO_COLL: {
            size_t cap = 64;
            char *out = malloc(cap);
            size_t len = 0;
#define VO_GROW(N) do { while (len + (N) + 1 >= cap) { cap *= 2; out = realloc(out, cap); } } while (0)
            VO_GROW(1); out[len++] = '[';
            for (int i = 0; i < v->count; i++) {
                char *piece = to_cstr(&v->items[i]);
                size_t plen = strlen(piece);
                VO_GROW((i > 0 ? 2 : 0) + plen);
                if (i > 0) { memcpy(out + len, ", ", 2); len += 2; }
                memcpy(out + len, piece, plen); len += plen;
                free(piece);
            }
            VO_GROW(1); out[len++] = ']'; out[len] = '\0';
#undef VO_GROW
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

void vo_show_part(VoValue *v) {
    char *s = to_cstr(v);
    printf("%s ", s);
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
        case VO_FILE: return v->file != NULL;
        case VO_TASK: return v->job_name != NULL;
        case VO_LOCK: return v->lock_held >= 0;
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
        case VO_FILE:
            return a->file == b->file &&
                   strcmp(a->file_mode ? a->file_mode : "", b->file_mode ? b->file_mode : "") == 0;
        case VO_TASK:
            return a->job_index == b->job_index && a->task_argc == b->task_argc;
        case VO_LOCK:
            return a->lock_held == b->lock_held;
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
    if (idx->type != VO_NUM) vo_error(line, "index must be NUM");
    if (coll->type == VO_TEX) {
        long len = (long)strlen(coll->tex ? coll->tex : "");
        if (idx->num < 0 || idx->num >= len) vo_error(line, "index out of range");
        char ch[2] = { coll->tex[idx->num], '\0' };
        if (dst->type == VO_TEX || dst->type == VO_COLL) vo_free(dst);
        vo_set_tex(dst, ch);
        return;
    }
    if (coll->type != VO_COLL) vo_error(line, "indexing needs a COLL");
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

/* ---- string helpers (mirror the interpreter's astr_* utilities) ---- */
static void vo_trim_ws(char *s) {
    char *end;
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
    int j = 0; while (s[i]) s[j++] = s[i++];
    s[j] = '\0';
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    *end = '\0';
}

static void vo_map_case(char *s, int upper) {
    for (; *s; s++) {
        if (upper && *s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
        else if (!upper && *s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static long vo_find(const char *hay, const char *ndl) {
    if (!hay || !ndl) return -1;
    if (ndl[0] == '\0') return 0;
    char *p = strstr(hay, ndl);
    return p ? (long)(p - hay) : -1;
}

void vo_sleep_ms(long ms) {
#ifdef _WIN32
    Sleep((DWORD)(ms < 0 ? 0 : ms));
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* ---- slices ---- */
void vo_slice(VoValue *out, VoValue *arr, VoValue *start, VoValue *end, int line) {
    long len = (arr->type == VO_TEX)
                 ? (long)strlen(arr->tex ? arr->tex : "")
                 : (arr->type == VO_COLL) ? arr->count : -1;
    if (len < 0) vo_error(line, "cannot slice a non-collection, non-text value");
    long s = (start->type == VO_EMP) ? 0 : (start->type == VO_NUM) ? start->num : (long)start->dec;
    long e = (end->type == VO_EMP) ? len : (end->type == VO_NUM) ? end->num : (long)end->dec;
    if (s < 0 || e < s || e > len) vo_error(line, "invalid slice");
    if (arr->type == VO_TEX) {
        char *sub = malloc((size_t)(e - s) + 1);
        memcpy(sub, arr->tex + s, (size_t)(e - s));
        sub[e - s] = '\0';
        if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
        vo_set_tex(out, sub);
        free(sub);
        return;
    }
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_coll_new(out);
    for (long i = s; i < e; i++) vo_coll_push(out, &arr->items[i], line);
}

/* ---- TEX interpolation ---- */
void vo_tex_interp_append_cstr(VoValue *acc, const char *s) {
    if (acc->type != VO_TEX) vo_set_tex(acc, "");
    size_t al = strlen(acc->tex), bl = strlen(s ? s : "");
    char *tmp = malloc(al + bl + 1);
    memcpy(tmp, acc->tex, al);
    memcpy(tmp + al, s ? s : "", bl);
    tmp[al + bl] = '\0';
    free(acc->tex);
    acc->tex = tmp;
}

void vo_tex_interp_append_val(VoValue *acc, VoValue *v) {
    char *piece = to_cstr(v);
    vo_tex_interp_append_cstr(acc, piece);
    free(piece);
}

/* =========================================================================
 * Builtin commands (v1.4) - mirror the tree-walking runtime's exec_command
 * semantics so the native and interpreter outputs match for the same
 * program.
 * ========================================================================= */

static long value_as_long(VoValue *v) { return v->type == VO_DEC ? (long)v->dec : v->num; }

void vo_cmd_attach(VoValue *coll, VoValue *item, int line) {
    if (coll->type != VO_COLL) vo_error(line, "ATTACH expects a collection reference");
    vo_coll_push(coll, item, line);
}

void vo_cmd_place(VoValue *coll, VoValue *idx, VoValue *item, int line) {
    if (coll->type != VO_COLL) vo_error(line, "PLACE expects a collection reference");
    long i = value_as_long(idx);
    if (i < 0 || i > coll->count) vo_error(line, "PLACE index out of bounds");
    if (coll->capacity < coll->count + 1) {
        coll->capacity = coll->capacity ? coll->capacity * 2 : 4;
        coll->items = realloc(coll->items, sizeof(VoValue) * coll->capacity);
    }
    memmove(coll->items + i + 1, coll->items + i, sizeof(VoValue) * (coll->count - i));
    vo_copy(&coll->items[i], item);
    coll->count++;
}

void vo_cmd_erase_coll(VoValue *coll, VoValue *idx, int line) {
    if (coll->type != VO_COLL) vo_error(line, "ERASE expects a collection reference");
    long i = value_as_long(idx);
    if (i < 0 || i >= coll->count) vo_error(line, "ERASE index out of bounds");
    vo_free(&coll->items[i]);
    memmove(coll->items + i, coll->items + i + 1, sizeof(VoValue) * (coll->count - i - 1));
    coll->count--;
}

void vo_cmd_erase_file(VoValue *path, int line) {
    char *p = to_cstr(path);
    int ok = remove(p) == 0;
    if (!ok) { fprintf(stderr, "[line %d] Runtime error: ERASE (file) failed on '%s'\n", line, p); free(p); exit(1); }
    free(p);
}

void vo_cmd_take(VoValue *out, VoValue *prompt, int line) {
    (void)line;
    char *ps = to_cstr(prompt);
    fputs(ps, stdout);
    fflush(stdout);
    free(ps);

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) buf[0] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';

    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);

    if (buf[0] != '\0') {
        char *end;
        long lval = strtol(buf, &end, 10);
        if (end != buf && *end == '\0') { vo_set_num(out, lval); return; }
        double dval = strtod(buf, &end);
        if (end != buf && *end == '\0') { vo_set_dec(out, dval); return; }
    }
    vo_set_tex(out, buf);
}

void vo_cmd_count(VoValue *out, VoValue *v, int line) {
    long n;
    if (v->type == VO_COLL) n = v->count;
    else if (v->type == VO_TEX) n = (long)strlen(v->tex ? v->tex : "");
    else vo_error(line, "COUNT expects a collection or text value");
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_num(out, n);
}

void vo_cmd_seek(VoValue *out, VoValue *hay, VoValue *ndl, int line) {
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    if (hay->type == VO_COLL) {
        long found = -1;
        for (int i = 0; i < hay->count; i++)
            if (values_equal(&hay->items[i], ndl)) { found = i; break; }
        if (found >= 0) vo_set_num(out, found); else vo_set_emp(out);
        return;
    }
    if (hay->type == VO_TEX) {
        char *hs = to_cstr(hay), *ns = to_cstr(ndl);
        long p = vo_find(hs, ns);
        if (p >= 0) vo_set_num(out, p); else vo_set_emp(out);
        free(hs); free(ns);
        return;
    }
    vo_error(line, "SEEK expects a collection or text value");
}

void vo_cmd_has_path(VoValue *out, VoValue *p, int line) {
    (void)line;
    char *path = to_cstr(p);
    FILE *f = fopen(path, "r");
    int exists = f != NULL;
    if (f) fclose(f);
    free(path);
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_yn(out, exists);
}

void vo_cmd_has(VoValue *out, VoValue *hay, VoValue *ndl, int line) {
    int found;
    if (hay->type == VO_COLL) {
        found = 0;
        for (int i = 0; i < hay->count; i++)
            if (values_equal(&hay->items[i], ndl)) { found = 1; break; }
    } else if (hay->type == VO_TEX) {
        char *hs = to_cstr(hay), *ns = to_cstr(ndl);
        found = vo_find(hs, ns) >= 0;
        free(hs); free(ns);
    } else {
        vo_error(line, "HAS expects a collection or text value");
        return;
    }
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_yn(out, found);
}

void vo_cmd_bind(VoValue *out, VoValue *coll, VoValue *sep, int line) {
    if (coll->type != VO_COLL) vo_error(line, "BIND expects a collection");
    char *seps = to_cstr(sep);
    size_t cap = 64, len = 0;
    char *o = malloc(cap); o[0] = '\0';
    size_t sl = strlen(seps);
    for (int i = 0; i < coll->count; i++) {
        char *piece = to_cstr(&coll->items[i]);
        size_t plen = strlen(piece);
        while (len + plen + sl + 2 > cap) { cap *= 2; o = realloc(o, cap); }
        if (i > 0) { memcpy(o + len, seps, sl); len += sl; }
        memcpy(o + len, piece, plen);
        len += plen;
        o[len] = '\0';
        free(piece);
    }
    free(seps);
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_tex(out, o);
    free(o);
}

void vo_cmd_sever(VoValue *out, VoValue *tex, VoValue *sep, int line) {
    if (tex->type != VO_TEX) vo_error(line, "SEVER expects a text value");
    char *seps = to_cstr(sep);
    if (seps[0] == '\0') { free(seps); vo_error(line, "SEVER separator must not be empty"); return; }
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_coll_new(out);
    char *p = tex->tex ? tex->tex : "";
    char *end = p + strlen(p);
    while (p <= end) {
        char *hit = strstr(p, seps);
        size_t clen = hit ? (size_t)(hit - p) : (size_t)(end - p);
        char *part = malloc(clen + 1);
        memcpy(part, p, clen);
        part[clen] = '\0';
        vo_coll_push(out, &(VoValue){0}, line);
        vo_set_tex(&out->items[out->count - 1], part);
        free(part);
        if (!hit) break;
        p = hit + strlen(seps);
    }
    free(seps);
}

void vo_cmd_cut(VoValue *out, VoValue *tex, int line) {
    if (tex->type != VO_TEX) vo_error(line, "CUT expects a text value");
    char *t = strdup(tex->tex ? tex->tex : "");
    vo_trim_ws(t);
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_tex(out, t);
    free(t);
}

void vo_cmd_case(VoValue *out, VoValue *tex, int upper, int line) {
    if (tex->type != VO_TEX) vo_errorf(line, "%s expects a text value", upper ? "RAISE" : "LOWER");
    char *t = strdup(tex->tex ? tex->tex : "");
    vo_map_case(t, upper);
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_tex(out, t);
    free(t);
}

void vo_cmd_unseal(VoValue *out, VoValue *path, VoValue *mode, int has_mode, int line) {
    char *p = to_cstr(path);
    const char *mode_s = "r";
    char own_mode[4]; own_mode[0] = '\0';
    if (has_mode) {
        char *ms = to_cstr(mode);
        if (strcmp(ms, "r") == 0) mode_s = "r";
        else if (strcmp(ms, "w") == 0) mode_s = "w";
        else if (strcmp(ms, "a") == 0) mode_s = "a";
        else if (strcmp(ms, "rw") == 0) mode_s = "r+";
        else vo_error(line, "invalid file mode");
        snprintf(own_mode, sizeof(own_mode), "%s", mode_s);
        free(ms);
    } else {
        snprintf(own_mode, sizeof(own_mode), "%s", mode_s);
    }
    FILE *f = fopen(p, mode_s);
    if (!f) vo_error(line, "UNSEAL failed");
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    out->type = VO_FILE;
    out->file = f;
    out->file_mode = strdup(own_mode);
    free(p);
}

void vo_cmd_seal(VoValue *f, int line) {
    if (f->type != VO_FILE || !f->file) vo_error(line, "SEAL expects a file value");
    fclose(f->file);
    f->file = NULL;
}

void vo_cmd_draw(VoValue *out, VoValue *f, VoValue *count, int has_count, int line) {
    if (f->type != VO_FILE || !f->file) vo_error(line, "DRAW expects an open file");
    long limit = -1;
    if (has_count) limit = value_as_long(count);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int ch;
    while ((limit < 0 || (long)len < limit) && (ch = fgetc(f->file)) != EOF) {
        if (len + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_set_tex(out, buf);
    free(buf);
}

void vo_cmd_put(VoValue *f, VoValue *t, int line) {
    if (f->type != VO_FILE || !f->file) vo_error(line, "PUT expects an open file");
    if (!f->file_mode || strcmp(f->file_mode, "r") == 0) vo_error(line, "PUT: file is not open for writing");
    char *s = to_cstr(t);
    size_t n = strlen(s);
    int ok = fwrite(s, 1, n, f->file) == n;
    free(s);
    if (!ok) vo_error(line, "PUT write failed");
}

void vo_cmd_move(VoValue *f, VoValue *pos, int line) {
    if (f->type != VO_FILE || !f->file) vo_error(line, "MOVE expects an open file");
    long p = value_as_long(pos);
    if (p < 0 || fseek(f->file, p, SEEK_SET) != 0) vo_error(line, "MOVE position invalid");
}

void vo_cmd_make(VoValue *p1, int line) {
    char *path = to_cstr(p1);
    FILE *f = fopen(path, "w");
    int ok = f != NULL;
    if (f) fclose(f);
    if (!ok) vo_error(line, "MAKE failed");
    free(path);
}

static void file_copy_move(VoValue *p1, VoValue *p2, int deliver, int line) {
    const char *cmd = deliver ? "DELIVER" : "CLONE";
    char msg[512];
    char *path1 = to_cstr(p1), *path2 = to_cstr(p2);
    FILE *src = fopen(path1, "rb");
    FILE *dst = fopen(path2, "wb");
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        snprintf(msg, sizeof(msg), "%s failed on '%s'", cmd, path1);
        free(path1); free(path2);
        vo_error(line, msg);
        return;
    }
    int ok = 1;
    char buf[8192]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0)
        if (fwrite(buf, 1, got, dst) != got) { ok = 0; break; }
    fclose(src); fclose(dst);
    if (ok && deliver) remove(path1);
    if (!ok) snprintf(msg, sizeof(msg), "%s failed on '%s'", cmd, path1);
    free(path1); free(path2);
    if (!ok) vo_error(line, msg);
}

void vo_cmd_recall(VoValue *p1, VoValue *p2, int line) {
    char *path1 = to_cstr(p1), *path2 = to_cstr(p2);
    if (rename(path1, path2) != 0) vo_error(line, "RECALL failed");
    free(path1); free(path2);
}

void vo_cmd_clone(VoValue *p1, VoValue *p2, int line) { file_copy_move(p1, p2, 0, line); }
void vo_cmd_deliver(VoValue *p1, VoValue *p2, int line) { file_copy_move(p1, p2, 1, line); }

/* ---- tasks / concurrency ---- */
void vo_cmd_spawn(VoValue *out, int job_index, int argc, int line) {
    if (job_index < 0 || job_index >= vo_job_tab_count) vo_error(line, "SPAWN: unknown job");
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    out->type = VO_TASK;
    out->job_name = strdup(vo_job_tab[job_index].name ? vo_job_tab[job_index].name : "?");
    out->job_index = job_index;
    out->task_argc = 0;
    out->task_args = argc ? malloc(sizeof(VoValue) * argc) : NULL;
    out->task_state = 0;
    out->task_result = NULL;
    for (int i = 0; i < argc; i++) { vo_set_null(&out->task_args[i]); out->task_argc = i + 1; }
    (void)line;
}

void vo_task_set_arg(VoValue *task, int i, VoValue *arg, int line) {
    if (task->type != VO_TASK) vo_error(line, "internal: task arg set on non-task");
    if (i < 0 || i >= task->task_argc) vo_error(line, "internal: task arg index out of range");
    vo_copy(&task->task_args[i], arg);
}

void vo_task_run(VoValue *task, int line) {
    if (task->type != VO_TASK) vo_error(line, "task expected");
    if (task->task_state == 2) vo_errorf(line, "task '%s' has been halted", task->job_name ? task->job_name : "?");
    if (task->task_state == 1) return;
    if (task->job_index < 0 || task->job_index >= vo_job_tab_count) vo_error(line, "invalid task job");
    VoJobInfo *j = &vo_job_tab[task->job_index];
    for (int i = 0; i < task->task_argc; i++) vo_assign(j->params[i], &task->task_args[i]);
    vo_set_emp(j->ret);
    j->fn();
    if (task->task_result) { vo_free(task->task_result); free(task->task_result); task->task_result = NULL; }
    task->task_result = malloc(sizeof(VoValue));
    vo_copy(task->task_result, j->ret);
    task->task_state = 1;
}

void vo_cmd_hold(VoValue *op, int line) {
    if (op->type == VO_TASK) { vo_task_run(op, line); return; }
    if (op->type == VO_NUM || op->type == VO_DEC) { vo_sleep_ms(value_as_long(op)); return; }
    vo_error(line, "HOLD expects a task or a numeric time in milliseconds");
}

void vo_cmd_claim(VoValue *out, VoValue *task, int line) {
    if (task->type != VO_TASK) vo_error(line, "CLAIM expects a task value");
    vo_task_run(task, line);
    if (out->type == VO_TEX || out->type == VO_COLL) vo_free(out);
    vo_copy(out, task->task_result);
}

void vo_cmd_halt(VoValue *task, int line) {
    if (task->type != VO_TASK) vo_error(line, "HALT expects a task");
    task->task_state = 2;
}

/* ---- locks/barriers ---- */
void vo_cmd_seize(VoValue *lock, int line) {
    if (lock->type != VO_LOCK) vo_error(line, "SEIZE expects a lock value");
    lock->lock_held = 1;
}
void vo_cmd_release(VoValue *lock, int line) {
    if (lock->type != VO_LOCK) vo_error(line, "RELEASE expects a lock value");
    if (!lock->lock_held) vo_error(line, "RELEASE: lock is not held (only the owner may release)");
    lock->lock_held = 0;
}
void vo_cmd_align(VoValue *barrier, int line) {
    if (barrier->type != VO_LOCK) vo_error(line, "ALIGN expects a barrier value");
    (void)line;
}
