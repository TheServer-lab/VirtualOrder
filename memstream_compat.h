#ifndef VO_MEMSTREAM_H
#define VO_MEMSTREAM_H
/* =========================================================================
 * open_memstream() is a glibc/BSD extension - it doesn't exist in MinGW's
 * C runtime, so native Windows builds (MSYS2 MinGW64, plain `gcc`) fail
 * to compile codegen.c without a replacement.
 *
 * codegen.c's usage is narrow and fixed: open a stream, write to it with
 * ordinary stdio calls, close it exactly once, then read the resulting
 * buffer/length. So rather than a fully general open_memstream (which
 * would need glibc's fopencookie or BSD's funopen - neither available on
 * Windows either), this shim backs the stream with a real temporary file
 * and reads its contents back into a heap buffer at close time. Close
 * must go through MEMSTREAM_CLOSE (not a bare fclose) so that readback
 * can happen; on platforms with a real open_memstream, MEMSTREAM_CLOSE
 * is just fclose and behaves exactly as before.
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32

static inline FILE *vo_open_memstream(char **bufp, size_t *sizep) {
    *bufp = NULL;
    *sizep = 0;
    return tmpfile();
}

static inline void vo_close_memstream(FILE *f, char **bufp, size_t *sizep) {
    long len = ftell(f);
    if (len < 0) len = 0;
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    size_t got = buf ? fread(buf, 1, (size_t)len, f) : 0;
    if (buf) buf[got] = '\0';
    fclose(f);
    *bufp = buf;
    *sizep = got;
}

#define open_memstream(bufp, sizep) vo_open_memstream((bufp), (sizep))
#define MEMSTREAM_CLOSE(f, bufp, sizep) vo_close_memstream((f), (bufp), (sizep))

#else

#define MEMSTREAM_CLOSE(f, bufp, sizep) ((void)(bufp), (void)(sizep), fclose(f))

#endif

#endif
