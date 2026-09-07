#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>   /* _spawnvp, _getpid */
#else
#include <unistd.h>
#include <libgen.h>
#include <sys/wait.h>
#endif

#include "parser.h"
#include "analyzer.h"
#include "codegen.h"

/* ---------------------------------------------------------------------
 * Locating the runtime objects (vo_rt.o + vo_entry.o)
 *
 * These are prebuilt once per target (they're the same for every .vo
 * program - see build.sh/Makefile) and linked into whatever native
 * binary voc produces, the same way crt0.o/libc get linked into every
 * C binary. Rather than hardcoding a shared /tmp path (races between
 * concurrent users/builds, doesn't survive install-to-a-different-
 * machine, and doesn't exist at all on Windows), voc finds them
 * relative to *its own* executable path, mirroring how a real
 * toolchain locates its support objects next to itself:
 *
 *   <exe_dir>/../lib/vo/<target>/vo_rt.o   (installed layout)
 *   <exe_dir>/vo_rt_<target>.o             (flat build/ layout)
 *
 * and only then falls back to compiling vo_rt.c/vo_entry.c straight
 * from source (useful for a dev checkout where nothing's been staged
 * into a lib dir yet).
 * ------------------------------------------------------------------- */
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void get_exe_dir(char *out, size_t out_size) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) { snprintf(out, out_size, "."); return; }
    char *slash = strrchr(buf, '\\');
    char *fwd   = strrchr(buf, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (!slash) { snprintf(out, out_size, "."); return; }
    *slash = '\0';
    snprintf(out, out_size, "%s", buf);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { snprintf(out, out_size, "."); return; }
    buf[n] = '\0';
    snprintf(out, out_size, "%s", dirname(buf));
#endif
}

typedef struct { char rt[4096]; char entry[4096]; int from_source; } RtObjects;

static int find_runtime_objects(CgTarget target, RtObjects *rt) {
    const char *tname = target == CG_TARGET_WINDOWS ? "windows" : "linux";
    char exe_dir[4096];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    char candidate_rt[4096], candidate_entry[4096];

    /* 1. installed layout: <exe_dir>/../lib/vo/<target>/{vo_rt,vo_entry}.o */
    snprintf(candidate_rt, sizeof(candidate_rt), "%s/../lib/vo/%s/vo_rt.o", exe_dir, tname);
    snprintf(candidate_entry, sizeof(candidate_entry), "%s/../lib/vo/%s/vo_entry.o", exe_dir, tname);
    if (file_exists(candidate_rt) && file_exists(candidate_entry)) {
        snprintf(rt->rt, sizeof(rt->rt), "%s", candidate_rt);
        snprintf(rt->entry, sizeof(rt->entry), "%s", candidate_entry);
        rt->from_source = 0;
        return 1;
    }

    /* 2. flat build/ layout: <exe_dir>/vo_rt_<target>.o */
    snprintf(candidate_rt, sizeof(candidate_rt), "%s/vo_rt_%s.o", exe_dir, tname);
    snprintf(candidate_entry, sizeof(candidate_entry), "%s/vo_entry_%s.o", exe_dir, tname);
    if (file_exists(candidate_rt) && file_exists(candidate_entry)) {
        snprintf(rt->rt, sizeof(rt->rt), "%s", candidate_rt);
        snprintf(rt->entry, sizeof(rt->entry), "%s", candidate_entry);
        rt->from_source = 0;
        return 1;
    }

    /* 3. dev fallback: compile straight from source, found next to the
       executable (source tree build) or in the current directory. */
    const char *dirs[2] = { exe_dir, "." };
    for (int i = 0; i < 2; i++) {
        snprintf(candidate_rt, sizeof(candidate_rt), "%s/vo_rt.c", dirs[i]);
        snprintf(candidate_entry, sizeof(candidate_entry), "%s/vo_entry.c", dirs[i]);
        if (file_exists(candidate_rt) && file_exists(candidate_entry)) {
            snprintf(rt->rt, sizeof(rt->rt), "%s", candidate_rt);
            snprintf(rt->entry, sizeof(rt->entry), "%s", candidate_entry);
            rt->from_source = 1;
            return 1;
        }
    }

    return 0;
}

/* Run `cc <argv...>` without going through a shell, so paths containing
   spaces or shell metacharacters (quotes, $, ;, ...) can't be
   misinterpreted or used to inject extra commands. */
static int run_cc(const char *cc, char **args, int argc) {
    char *argv[64];
    int n = 0;
    argv[n++] = (char *)cc;
    for (int i = 0; i < argc && n < 62; i++) argv[n++] = args[i];
    argv[n] = NULL;

#ifdef _WIN32
    /* No fork() on Windows; _spawnvp launches + waits for the child in
       one call and hands back its exit code directly. */
    intptr_t rc = _spawnvp(_P_WAIT, cc, (const char * const *)argv);
    if (rc == -1) { fprintf(stderr, "failed to run '%s': ", cc); perror(NULL); return -1; }
    return (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(cc, argv);
        fprintf(stderr, "failed to run '%s': ", cc);
        perror(NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (fread(buf, 1, size, f) != (size_t)size) { perror("fread"); exit(1); }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void usage(const char *prog) {
#ifdef _WIN32
    const char *host_default = "windows";
#else
    const char *host_default = "linux";
#endif
    fprintf(stderr,
        "usage: %s <file.vo> [-S] [-o OUTPUT] [--target=linux|windows]\n"
        "  -S               emit x86-64 assembly (.s) instead of a binary\n"
        "  -o OUTPUT        output path (default: a.out / a.exe, or <file>.s with -S)\n"
        "  --target=TARGET  linux or windows (default on this build: %s).\n"
        "                   the non-default target needs a matching cross\n"
        "                   toolchain (x86_64-w64-mingw32-gcc for windows,\n"
        "                   x86_64-linux-gnu-gcc for linux) on your PATH.\n",
        prog, host_default);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *vo_path = NULL;
    const char *out_path = NULL;
    int emit_asm_only = 0;
    /* Default to whatever ABI this copy of voc's own host compiler
       produces natively: System V on Linux, Windows x64 on Windows.
       Getting this wrong doesn't fail to link - it links fine and then
       corrupts every function call's argument registers at runtime,
       so it's worth defaulting correctly rather than always picking
       "linux". */
#ifdef _WIN32
    CgTarget target = CG_TARGET_WINDOWS;
#else
    CgTarget target = CG_TARGET_LINUX;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) emit_asm_only = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *t = argv[i] + 9;
            if (strcmp(t, "windows") == 0) target = CG_TARGET_WINDOWS;
            else if (strcmp(t, "linux") == 0) target = CG_TARGET_LINUX;
            else { fprintf(stderr, "unknown target '%s'\n", t); return 1; }
        } else if (!vo_path) vo_path = argv[i];
        else { usage(argv[0]); return 1; }
    }
    if (!vo_path) { usage(argv[0]); return 1; }

    char *source = read_file(vo_path);
    Parser parser;
    parser_init(&parser, source);
    ASTNode *program = parser_parse_program(&parser);
    if (parser.had_error) {
        fprintf(stderr, "\nParsing finished with errors; not compiling.\n");
        free(source);
        return 1;
    }
    if (analyze_program(program)) {
        fprintf(stderr, "\nSemantic analysis finished with errors; not compiling.\n");
        free(source);
        return 1;
    }

    char asm_path[4096];
    if (emit_asm_only) {
        snprintf(asm_path, sizeof(asm_path), "%s", out_path ? out_path : "out.s");
    } else {
#ifdef _WIN32
        char tmp_dir[MAX_PATH];
        DWORD tn = GetTempPathA(sizeof(tmp_dir), tmp_dir);
        if (tn == 0 || tn >= sizeof(tmp_dir)) snprintf(tmp_dir, sizeof(tmp_dir), ".\\");
        snprintf(asm_path, sizeof(asm_path), "%svoc_%u.s", tmp_dir, (unsigned)_getpid());
#else
        snprintf(asm_path, sizeof(asm_path), "/tmp/voc_%d.s", getpid());
#endif
    }

    FILE *out = fopen(asm_path, "w");
    if (!out) { perror("fopen"); free(source); return 1; }
    int rc = codegen_compile(program, target, out);
    fclose(out);

    if (rc != 0) {
        fprintf(stderr, "\nNative code generation finished with errors.\n");
        if (!emit_asm_only) remove(asm_path);
        free(source);
        return 1;
    }

    if (emit_asm_only) {
        fprintf(stderr, "wrote %s\n", asm_path);
        free(source);
        return 0;
    }

    /* Assemble + link asm_path together with vo_rt.o and the tiny C main()
       stub (vo_entry.c) that just calls vo_main(). We shell out to the
       target's C compiler/linker driver so we don't have to reimplement
       ELF/PE object emission ourselves - same division of labor as any
       compiler that emits .s and hands it to `as`/`ld` via `cc`. */
    const char *default_out = target == CG_TARGET_WINDOWS ? "a.exe" : "a.out";
    const char *final_out = out_path ? out_path : default_out;
    /* Pick the compiler that actually produces the requested target's
       ABI. On a Linux host, plain "gcc" is System V/ELF and windows
       needs the mingw-w64 cross name; on a Windows host (native MSYS2/
       MinGW64 build of voc itself), it's the other way around - plain
       "gcc" there already IS the Windows x64/PE compiler, and "linux"
       would be the cross case (rare, needs its own cross toolchain). */
#ifdef _WIN32
    const char *cc = target == CG_TARGET_WINDOWS ? "gcc" : "x86_64-linux-gnu-gcc";
#else
    const char *cc = target == CG_TARGET_WINDOWS ? "x86_64-w64-mingw32-gcc" : "gcc";
#endif

    RtObjects rt;
    if (!find_runtime_objects(target, &rt)) {
        fprintf(stderr,
            "error: couldn't find the '%s' runtime objects (vo_rt.o/vo_entry.o) "
            "installed next to voc, nor vo_rt.c/vo_entry.c to build them from "
            "source. Run build.sh / `make install` first.\n",
            target == CG_TARGET_WINDOWS ? "windows" : "linux");
        remove(asm_path);
        free(source);
        return 1;
    }

    char *args[16];
    int n = 0;
    args[n++] = "-O2";
    /* -no-pie and -Wl,-z,noexecstack are ELF/Linux linker concepts;
       Windows PE has no equivalent and some mingw ld versions reject
       unrecognized flags outright, so only pass these for the ELF
       target. */
    if (target != CG_TARGET_WINDOWS) {
        args[n++] = "-no-pie";
        args[n++] = "-Wl,-z,noexecstack";
    }
    args[n++] = "-o";
    args[n++] = (char *)final_out;
    args[n++] = asm_path;
    args[n++] = rt.rt;
    args[n++] = rt.entry;
    args[n++] = "-lm";

    if (rt.from_source) {
        fprintf(stderr, "note: no prebuilt runtime objects found; compiling %s and %s from source\n",
                rt.rt, rt.entry);
    }

    int status = run_cc(cc, args, n);
    remove(asm_path);
    free(source);
    if (status != 0) {
        fprintf(stderr, "assemble/link failed (is '%s' installed%s?)\n",
                cc, target == CG_TARGET_WINDOWS ? " - needs a mingw-w64 cross toolchain" : "");
        return 1;
    }
    fprintf(stderr, "wrote %s\n", final_out);
    return 0;
}
