#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include "wasm3.h"
#include "m3_env.h"
#include "platform.h"
#include "yos/types.h"
#include "yos/ydebug.h"
#include "yos/vfs/mount.h"
#include "yos/vfs/procfs.h"
#include "impl/pthread.h"

/* Legacy yos-private number-indexed dispatcher REMOVED — see CLAUDE.md
 * non-negotiable #5. The wasm guest must import each libc function by
 * name (env.read, env.write, …) and `yos_brg_link_imports()` from
 * the auto-generated bridges resolves them. */

/* Variadic libc family — printf/fprintf/sprintf/snprintf and the v*
 * variants. Wasm has no `...` so clang lowers each call to a non-
 * variadic call (fmt_off, va_list_off). impl/printf.c walks fmt and
 * reads va_list from guest memory. The m3w_<name> trampolines below
 * only differ in argument unpacking; all delegate to one of the
 * yos_*printf functions. */
extern int32_t yos_printf  (struct yos_exec_ctx *ctx,
                            uint32_t fmt_off, uint32_t va_off);
extern int32_t yos_fprintf (struct yos_exec_ctx *ctx,
                            uint32_t fp, uint32_t fmt_off, uint32_t va_off);
extern int32_t yos_sprintf (struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t fmt, uint32_t va);
extern int32_t yos_snprintf(struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t n, uint32_t fmt, uint32_t va);
extern int32_t yos_vprintf (struct yos_exec_ctx *ctx,
                            uint32_t fmt, uint32_t va);
extern int32_t yos_vfprintf(struct yos_exec_ctx *ctx,
                            uint32_t fp, uint32_t fmt, uint32_t va);
extern int32_t yos_vsprintf(struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t fmt, uint32_t va);
extern int32_t yos_vsnprintf(struct yos_exec_ctx *ctx,
                             uint32_t dst, uint32_t n, uint32_t fmt, uint32_t va);

/* exec family — must trap to unwind out of wasm so the host's exec
 * pump can load the new module. yos_execve* set exec_pending=1 and
 * return 0; if we just RETURN that 0 to wasm, libc/libuv's caller
 * sees "execvp returned successfully" which by POSIX means it
 * FAILED (real execvp never returns), so libuv writes errno into
 * its error pipe and _exit(127)s the child without giving us a
 * chance to load the new module. Trapping here stops wasm execution
 * immediately; the host loop sees exec_pending and proceeds to
 * load the new module — `res` from the trap is discarded when
 * exec_pending is true. */
extern int32_t yos_execvp(struct yos_exec_ctx *ctx, uint32_t file, uint32_t argv_ptr);
extern int32_t yos_execv (struct yos_exec_ctx *ctx, uint32_t path, uint32_t argv_ptr);
extern int32_t yos_execve(struct yos_exec_ctx *ctx, uint32_t file, uint32_t argv, uint32_t envp);
extern int32_t yos_execvpe(struct yos_exec_ctx *ctx, uint32_t file, uint32_t argv, uint32_t envp);

/* Helper: refresh ctx->memory for the wrappers below. */
static inline void pfx_refresh(IM3Runtime rt, struct yos_exec_ctx *ctx) {
    uint32_t ms = 0;
    ctx->memory = m3_GetMemory(rt, &ms, 0);
    ctx->memory_size = ms;
}

/* Forward decls — definitions further down. The variadic trampolines need
 * to feed the bridge ring buffer too so YOS_BRG_TRACE / crash dumps see
 * printf-family activity. */
extern const char *yos_brg_last_call;
extern int yos_brg_trace;
extern void yos_brg_record(const char *name);
#define PFX_TRACE(name) do {                                       \
    yos_brg_last_call = (name); yos_brg_record(name);              \
    if (yos_brg_trace) fprintf(stderr, "yos_brg: %s\n", (name));   \
} while (0)

static m3ApiRawFunction(m3_printf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("printf");
    m3ApiReturn(yos_printf(ctx, fmt, va));
}
static m3ApiRawFunction(m3_fprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fp); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("fprintf");
    m3ApiReturn(yos_fprintf(ctx, fp, fmt, va));
}
static m3ApiRawFunction(m3_sprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("sprintf");
    m3ApiReturn(yos_sprintf(ctx, dst, fmt, va));
}
static m3ApiRawFunction(m3_snprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("snprintf");
    m3ApiReturn(yos_snprintf(ctx, dst, n, fmt, va));
}
static m3ApiRawFunction(m3_vprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("vprintf");
    m3ApiReturn(yos_vprintf(ctx, fmt, va));
}
static m3ApiRawFunction(m3_vfprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fp); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("vfprintf");
    m3ApiReturn(yos_vfprintf(ctx, fp, fmt, va));
}
static m3ApiRawFunction(m3_vsprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_vsprintf(ctx, dst, fmt, va));
}
static m3ApiRawFunction(m3_vsnprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("vsnprintf");
    m3ApiReturn(yos_vsnprintf(ctx, dst, n, fmt, va));
}

/* exec family — trap on success so libuv's child can't fall through
 * to its post-execvp "exec failed" error-pipe + _exit(127) path.
 * yos_execve* set exec_pending=1 and return 0; that 0 looks like
 * a successful return from execvp() to libc/libuv, which by POSIX
 * means it FAILED (real execvp never returns). Trapping here stops
 * wasm execution immediately; the host loop checks exec_pending
 * before treating the trap as a real error and proceeds to load
 * the new module. */
static m3ApiRawFunction(m3_execvp) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, file); m3ApiGetArg(uint32_t, argv);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("execvp");
    int32_t rc = yos_execvp(ctx, file, argv);
    if (ctx->exec_pending) m3ApiTrap("exec");
    m3ApiReturn(rc);
}
static m3ApiRawFunction(m3_execv) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, path); m3ApiGetArg(uint32_t, argv);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("execv");
    int32_t rc = yos_execv(ctx, path, argv);
    if (ctx->exec_pending) m3ApiTrap("exec");
    m3ApiReturn(rc);
}
static m3ApiRawFunction(m3_execve) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, file); m3ApiGetArg(uint32_t, argv);
    m3ApiGetArg(uint32_t, envp);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("execve");
    int32_t rc = yos_execve(ctx, file, argv, envp);
    if (ctx->exec_pending) m3ApiTrap("exec");
    m3ApiReturn(rc);
}
static m3ApiRawFunction(m3_execvpe) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, file); m3ApiGetArg(uint32_t, argv);
    m3ApiGetArg(uint32_t, envp);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    PFX_TRACE("execvpe");
    int32_t rc = yos_execvpe(ctx, file, argv, envp);
    if (ctx->exec_pending) m3ApiTrap("exec");
    m3ApiReturn(rc);
}

/* env.__main_argc_argv: clang's wasm32 command-exec-model renames a
 * user-defined `int main(int, char**)` to `__main_argc_argv` and emits
 * a wrapper `int main(void)` that's exported. Our crt1's call to
 * `main(argc, argv)` therefore lowers to env.__main_argc_argv. We
 * resolve it host-side to the wasm module's *real* main implementation
 * — which is exported under both names depending on linker behaviour
 * but reachable as func "main" via m3_FindFunction. The lookup is
 * lazy on first call (the module is fully loaded by then). */
static m3ApiRawFunction(m3_main_argc_argv)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, argc);
    m3ApiGetArg(int32_t, argv);

    /* Diagnostic: trace the argv nvim is receiving. Only printed when
     * the project-wide trace switch (YTRACE_DEFAULT_ON=yes) is on —
     * normal runs stay quiet. */
    if (ydebug_enabled()) {
        struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
        uint32_t ms = 0;
        ctx->memory = m3_GetMemory(runtime, &ms, 0);
        ctx->memory_size = ms;
        ydebug("main(argc=%d, argv=0x%x)\n", argc, argv);
        uint32_t *av = (uint32_t *)(ctx->memory + (uint32_t)argv);
        for (int i = 0; i < argc && i < 4; i++) {
            uint32_t s_off = av[i];
            const char *s = (s_off && s_off < ms)
                          ? (const char *)(ctx->memory + s_off)
                          : "<bad>";
            ydebug("  argv[%d] off=0x%x \"%s\"\n", i, s_off, s);
        }
    }

    /* MUST be a per-runtime lookup, not static. After fork the child has
     * its OWN runtime with its OWN compiled `main` function pointer; a
     * static cache populated by the parent thread's first call would make
     * the child invoke the parent's compiled main, which silently runs in
     * the wrong runtime context (asyncify state, globals, memory all from
     * the wrong runtime). Symptom: the child appears to run main "fresh"
     * after fork instead of rewinding to the captured fork callsite, and
     * never reaches its post-fork execvp(). */
    IM3Function f_main = NULL;
    M3Result r = m3_FindFunction(&f_main, runtime, "__main_argc_argv");
    if (r || !f_main) r = m3_FindFunction(&f_main, runtime, "main");
    if (r || !f_main) {
        fprintf(stderr, "yos: __main_argc_argv: no main in module\n");
        m3ApiReturn(-1);
    }
    r = m3_CallV(f_main, argc, argv);
    if (r) {
        /* Intentional traps (exec) are part of normal control flow, not
         * errors — the host loop catches them via exec_pending and loads
         * the new module. PROPAGATE the trap up through m3_CallV so the
         * outer fork_thread_func / main exec loop sees res != NULL,
         * checks exec_pending, and proceeds to reload. If we absorbed
         * the trap here (m3ApiReturn), wasm's _start would resume,
         * call exit(-1), pthread_exit on the child thread, and the
         * exec-pending check in the host loop would never fire. */
        struct yos_exec_ctx *ctx2 =
            (struct yos_exec_ctx *)m3_GetUserData(runtime);
        if (ctx2 && ctx2->exec_pending) {
            return r;  /* trap propagates up; host loop handles exec */
        }
        fprintf(stderr, "yos: main trapped: %s\n", r);
        IM3BacktraceInfo bt = m3_GetBacktrace(runtime);
        if (bt && bt->frames) {
            IM3BacktraceFrame f = bt->frames;
            int i = 0;
            while (f && i < 32) {
                const char *fn = f->function ? m3_GetFunctionName(f->function) : "?";
                fprintf(stderr, "yos: bt #%d %s\n", i++, fn);
                f = f->next;
            }
        }
        m3ApiReturn(-1);
    }
    int32_t rc = 0;
    m3_GetResultsV(f_main, &rc);
    m3ApiReturn(rc);
}

/* env.__error: FreeBSD's errno accessor — `int *__error(void)`. The
 * FreeBSD <errno.h> macro `#define errno (*__error())` lowers every
 * `errno` reference to a call here. We return the wasm offset of the
 * per-ctx errno slot reserved at load time; bridges write the mapped
 * host errno into that slot on error paths. */
static m3ApiRawFunction(m3_yos_error)
{
    m3ApiReturnType(int32_t);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    m3ApiReturn((int32_t)ctx->errno_off);
}

/* Last bridge call name — set by every generated m3w_<X> wrapper before
 * dispatching. main.c's abort/assert handlers print it so we know which
 * libc function nvim called immediately before bailing out. */
const char *yos_brg_last_call = "<none>";

/* Set to 1 to log every bridge call. Pulled from YOS_BRG_TRACE env var
 * in main(). Useful for debugging "what did the wasm just try to call"
 * crashes. Very noisy — disable for normal runs. */
int yos_brg_trace = 0;

/* Ring buffer of the last N bridge calls per host thread. Each m3w_*
 * wrapper calls yos_brg_record(name); on a __stack_chk_fail trap we
 * dump the buffer so we can see which bridges (and which thread) led
 * up to the corruption. Cross-thread bridge interleaving is the
 * primary suspect for the kind of canary smashes nvim hits in the
 * channel_job_start path — the parent and the asyncify-fork "child"
 * both run host glibc concurrently, and any FILE-table or stdio
 * race shows up as a smashed canary later. */
#define YOS_BRG_RING 1024
struct yos_brg_rec {
    const char *name;
    pid_t       tid;
    uint64_t    seq;
    uint64_t    args[4];          /* up to 4 first wasm-ABI args (raw u32 widened) */
};
static struct yos_brg_rec yos_brg_ring[YOS_BRG_RING];
static _Atomic uint64_t   yos_brg_ring_seq = 0;
/* Most-recently-recorded slot, so per-bridge `_sp` peeks land in the
 * right slot. yos_brg_record advances the global sequence and
 * stores its index here for the bridge to fill in args. */
static __thread uint32_t  yos_brg_my_slot = 0;

static inline pid_t yos_brg_gettid(void) {
    return yos_plat_gettid();
}

void yos_brg_record(const char *name)
{
    uint64_t s = atomic_fetch_add(&yos_brg_ring_seq, 1);
    struct yos_brg_rec *r = &yos_brg_ring[s % YOS_BRG_RING];
    r->name = name;
    r->tid  = yos_brg_gettid();
    r->seq  = s;
    r->args[0] = r->args[1] = r->args[2] = r->args[3] = 0;
    yos_brg_my_slot = (uint32_t)(s % YOS_BRG_RING);
}

void yos_brg_record_args(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct yos_brg_rec *r = &yos_brg_ring[yos_brg_my_slot];
    r->args[0] = a0; r->args[1] = a1; r->args[2] = a2; r->args[3] = a3;
}

/* Dump a host-side string buffer at a wasm offset, escaping non-print
 * chars; used to interpret pointer-shaped bridge args. */
static void yos_brg_dump_strarg(FILE *out, struct yos_exec_ctx *ctx,
                                uint64_t off, int max)
{
    if (!ctx || !ctx->memory || off == 0 || off >= ctx->memory_size) {
        fprintf(out, "<%lx>", (unsigned long)off);
        return;
    }
    fputc('"', out);
    const char *p = (const char *)(ctx->memory + off);
    for (int i = 0; i < max && p[i]; i++) {
        unsigned c = (unsigned char)p[i];
        if (c >= 32 && c < 127) fputc((int)c, out);
        else fprintf(out, "\\x%02x", c);
    }
    fputc('"', out);
}

static void yos_brg_dump_ring(FILE *out, int n, struct yos_exec_ctx *ctx)
{
    uint64_t end = atomic_load(&yos_brg_ring_seq);
    if (end == 0) { fprintf(out, "yos:   (no bridge calls recorded)\n"); return; }
    if (n > YOS_BRG_RING) n = YOS_BRG_RING;
    if ((uint64_t)n > end) n = (int)end;
    fprintf(out, "yos: last %d bridge calls (most recent first):\n", n);
    for (int i = 0; i < n; i++) {
        uint64_t s = end - 1 - i;
        struct yos_brg_rec *r = &yos_brg_ring[s % YOS_BRG_RING];
        fprintf(out, "yos:   #%u tid=%d %-12s a0=%lx a1=%lx a2=%lx a3=%lx",
                (unsigned)(end - r->seq), (int)r->tid,
                r->name ? r->name : "?",
                (unsigned long)r->args[0],
                (unsigned long)r->args[1],
                (unsigned long)r->args[2],
                (unsigned long)r->args[3]);
        /* Common cases: fopen path, getenv name, fclose handle. */
        if (r->name && (!strcmp(r->name, "fopen") || !strcmp(r->name, "getenv")
                     || !strcmp(r->name, "open") || !strcmp(r->name, "stat"))) {
            fputc(' ', out);
            yos_brg_dump_strarg(out, ctx, r->args[0], 80);
        }
        fputc('\n', out);
    }
}

/* Set by main() so the SIGBUS/SIGSEGV/SIGILL handler can pretty-print
 * the recent bridge ring when the host crashes inside a libc call. */
struct yos_exec_ctx *yos_brg_dump_ctx = NULL;

void yos_host_crash_handler_si(int sig, siginfo_t *si, void *uctx)
{
    (void)uctx;
    int fd = open("/tmp/yos-host-crash.log",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[512];
        int n = snprintf(buf, sizeof buf,
                         "yos: HOST CRASH signal=%d code=%d addr=%p "
                         "last_bridge=%s\n",
                         sig, si ? si->si_code : 0,
                         si ? si->si_addr : NULL,
                         yos_brg_last_call ? yos_brg_last_call : "<none>");
        if (n > 0) (void)!write(fd, buf, (size_t)n);
        uint64_t end = atomic_load(&yos_brg_ring_seq);
        int dump = 30;
        if ((uint64_t)dump > end) dump = (int)end;
        for (int i = 0; i < dump; i++) {
            uint64_t s = end - 1 - i;
            struct yos_brg_rec *r = &yos_brg_ring[s % YOS_BRG_RING];
            n = snprintf(buf, sizeof buf,
                         "  #%d tid=%d %-14s a0=%lx a1=%lx a2=%lx a3=%lx\n",
                         (int)(end - r->seq), (int)r->tid,
                         r->name ? r->name : "?",
                         (unsigned long)r->args[0],
                         (unsigned long)r->args[1],
                         (unsigned long)r->args[2],
                         (unsigned long)r->args[3]);
            if (n > 0) (void)!write(fd, buf, (size_t)n);
        }
        fsync(fd);
        (void)close(fd);
    }
    _exit(128 + sig);
}

/* SIGUSR1 → dump the recent bridge ring (last N calls with tid) to
 * /tmp/yos-host-ring.log. async-signal-safe enough — only open/write
 * and atomic reads. Used to inspect parallel-process state when nvim
 * hangs without crashing. */
void yos_brg_dump_handler(int sig)
{
    (void)sig;
    int fd = open("/tmp/yos-host-ring.log",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[256];
    uint64_t end = atomic_load(&yos_brg_ring_seq);
    int n = snprintf(buf, sizeof buf,
                     "yos: bridge ring dump end_seq=%llu last_call=%s\n",
                     (unsigned long long)end,
                     yos_brg_last_call ? yos_brg_last_call : "<none>");
    if (n > 0) (void)!write(fd, buf, (size_t)n);
    int dump = 80;
    if ((uint64_t)dump > end) dump = (int)end;
    for (int i = 0; i < dump; i++) {
        uint64_t s = end - 1 - i;
        struct yos_brg_rec *r = &yos_brg_ring[s % YOS_BRG_RING];
        n = snprintf(buf, sizeof buf,
                     "  -%3d tid=%d %-14s a0=%lx a1=%lx a2=%lx a3=%lx\n",
                     i, (int)r->tid,
                     r->name ? r->name : "?",
                     (unsigned long)r->args[0],
                     (unsigned long)r->args[1],
                     (unsigned long)r->args[2],
                     (unsigned long)r->args[3]);
        if (n > 0) (void)!write(fd, buf, (size_t)n);
    }
    fsync(fd);
    (void)close(fd);
}

#ifdef __APPLE__
/* ── Darwin Mach exception handler ──────────────────────────────────
 *
 * On macOS, synchronous CPU faults (illegal instruction, bad access,
 * arithmetic) are routed through the Mach exception subsystem BEFORE
 * the BSD signal layer ever runs. If no Mach handler is registered,
 * the kernel hands the exception to ReportCrash and the process dies
 * with the BSD signal recorded in waitpid(2)'s status, but our POSIX
 * sigaction handler is never invoked — we get no register dump, no
 * faulting PC, no diagnostic at all.
 *
 * Catching it requires:
 *   1. A Mach port to receive exception messages on
 *   2. task_set_exception_ports() to route synchronous faults there
 *   3. A dedicated thread that mach_msg-loops on the port, dumps the
 *      faulting thread's register state, then _exit()s.
 */

#include <mach/mach.h>
#include <mach/exception_types.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <dlfcn.h>

/* Layout of an EXCEPTION_DEFAULT|MACH_EXCEPTION_CODES request, mirrors
 * what MIG would generate from mach_exc.defs. We avoid pulling in MIG
 * so the build doesn't need a generated stub library. */
typedef struct {
    mach_msg_header_t           Head;
    mach_msg_body_t             msgh_body;
    mach_msg_port_descriptor_t  thread;
    mach_msg_port_descriptor_t  task;
    NDR_record_t                NDR;
    exception_type_t            exception;
    mach_msg_type_number_t      codeCnt;
    int64_t                     code[2];
    char                        pad[64];   /* trailer + slack */
} yos_mach_exc_request_t;

static mach_port_t yos_mach_exc_port = MACH_PORT_NULL;

static void yos_mach_dump_crash(int sig, exception_type_t exc,
                                int64_t code0, int64_t code1,
                                mach_port_t thread)
{
    int fd = open("/tmp/yos-host-crash.log",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "yos: HOST CRASH (mach) signal=%d exc=%d "
                     "code0=0x%llx code1=0x%llx last_bridge=%s\n",
                     sig, (int)exc,
                     (long long)code0, (long long)code1,
                     yos_brg_last_call ? yos_brg_last_call : "<none>");
    if (n > 0) (void)!write(fd, buf, (size_t)n);

    /* Resolve faulting address (code1) via dladdr — tells us which dylib
     * / function the crashing thread was inside. */
    {
        Dl_info dli;
        if (code1 && dladdr((void *)(uintptr_t)code1, &dli)) {
            n = snprintf(buf, sizeof buf,
                         "  code1.dli: file=%s sym=%s sym_addr=%p base=%p\n",
                         dli.dli_fname ? dli.dli_fname : "?",
                         dli.dli_sname ? dli.dli_sname : "?",
                         dli.dli_saddr, dli.dli_fbase);
            if (n > 0) (void)!write(fd, buf, (size_t)n);
        }
    }

#if defined(__x86_64__)
    x86_thread_state64_t st;
    mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64,
                         (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
        n = snprintf(buf, sizeof buf,
            "  rip=0x%llx rflags=0x%llx\n"
            "  rsp=0x%llx rbp=0x%llx\n"
            "  rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n"
            "  rdi=0x%llx rsi=0x%llx  r8=0x%llx  r9=0x%llx\n"
            "  r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx\n"
            "  r14=0x%llx r15=0x%llx\n",
            (unsigned long long)st.__rip, (unsigned long long)st.__rflags,
            (unsigned long long)st.__rsp, (unsigned long long)st.__rbp,
            (unsigned long long)st.__rax, (unsigned long long)st.__rbx,
            (unsigned long long)st.__rcx, (unsigned long long)st.__rdx,
            (unsigned long long)st.__rdi, (unsigned long long)st.__rsi,
            (unsigned long long)st.__r8,  (unsigned long long)st.__r9,
            (unsigned long long)st.__r10, (unsigned long long)st.__r11,
            (unsigned long long)st.__r12, (unsigned long long)st.__r13,
            (unsigned long long)st.__r14, (unsigned long long)st.__r15);
        if (n > 0) (void)!write(fd, buf, (size_t)n);

        /* Dump 32 bytes at faulting PC, useful to spot a 0xCC int3 or
         * a 0x0f 0x0b ud2 (clang's __builtin_trap). */
        unsigned char ib[32];
        if (st.__rip) {
            vm_size_t got = 0;
            kern_return_t kr = vm_read_overwrite(
                mach_task_self(), (vm_address_t)st.__rip,
                sizeof ib, (vm_address_t)ib, &got);
            if (kr == KERN_SUCCESS && got > 0) {
                int off = snprintf(buf, sizeof buf, "  bytes@rip:");
                for (vm_size_t i = 0; i < got && off + 4 < (int)sizeof buf; i++) {
                    off += snprintf(buf + off, sizeof buf - off,
                                    " %02x", ib[i]);
                }
                if (off + 1 < (int)sizeof buf) buf[off++] = '\n';
                (void)!write(fd, buf, (size_t)off);
            }
        }
        /* Resolve RIP and RAX through dladdr too — pinpoint the
         * crashing function and the indirect-call target. */
        Dl_info dli;
        if (st.__rip && dladdr((void *)(uintptr_t)st.__rip, &dli)) {
            n = snprintf(buf, sizeof buf,
                         "  rip.dli: file=%s sym=%s sym_addr=%p base=%p off=0x%lx\n",
                         dli.dli_fname ? dli.dli_fname : "?",
                         dli.dli_sname ? dli.dli_sname : "?",
                         dli.dli_saddr, dli.dli_fbase,
                         (unsigned long)((uintptr_t)st.__rip
                             - (uintptr_t)dli.dli_saddr));
            if (n > 0) (void)!write(fd, buf, (size_t)n);
        }
        if (st.__rax && dladdr((void *)(uintptr_t)st.__rax, &dli)) {
            n = snprintf(buf, sizeof buf,
                         "  rax.dli: file=%s sym=%s sym_addr=%p base=%p\n",
                         dli.dli_fname ? dli.dli_fname : "?",
                         dli.dli_sname ? dli.dli_sname : "?",
                         dli.dli_saddr, dli.dli_fbase);
            if (n > 0) (void)!write(fd, buf, (size_t)n);
        }
    }
#elif defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64,
                         (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
        n = snprintf(buf, sizeof buf,
            "  pc=0x%llx sp=0x%llx fp=0x%llx lr=0x%llx cpsr=0x%x\n",
            (unsigned long long)__darwin_arm_thread_state64_get_pc(st),
            (unsigned long long)__darwin_arm_thread_state64_get_sp(st),
            (unsigned long long)__darwin_arm_thread_state64_get_fp(st),
            (unsigned long long)__darwin_arm_thread_state64_get_lr(st),
            (unsigned)st.__cpsr);
        if (n > 0) (void)!write(fd, buf, (size_t)n);
        for (int i = 0; i < 29; i += 4) {
            n = snprintf(buf, sizeof buf,
                "  x%02d=0x%llx x%02d=0x%llx x%02d=0x%llx x%02d=0x%llx\n",
                i,   (unsigned long long)st.__x[i],
                i+1, (unsigned long long)st.__x[i+1],
                i+2, (unsigned long long)st.__x[i+2],
                i+3, (unsigned long long)st.__x[i+3]);
            if (n > 0) (void)!write(fd, buf, (size_t)n);
        }
    }
#endif

    /* Dump the recent bridge ring — tells us which libc bridge the
     * guest had just called. */
    uint64_t end = atomic_load(&yos_brg_ring_seq);
    int dump = 30;
    if ((uint64_t)dump > end) dump = (int)end;
    for (int i = 0; i < dump; i++) {
        uint64_t s = end - 1 - i;
        struct yos_brg_rec *r = &yos_brg_ring[s % YOS_BRG_RING];
        n = snprintf(buf, sizeof buf,
                     "  #%d tid=%d %-14s a0=%lx a1=%lx a2=%lx a3=%lx\n",
                     (int)(end - r->seq), (int)r->tid,
                     r->name ? r->name : "?",
                     (unsigned long)r->args[0],
                     (unsigned long)r->args[1],
                     (unsigned long)r->args[2],
                     (unsigned long)r->args[3]);
        if (n > 0) (void)!write(fd, buf, (size_t)n);
    }
    fsync(fd);
    (void)close(fd);
}

static void *yos_mach_exc_thread(void *arg)
{
    (void)arg;
    /* Don't let this thread itself be caught by our own port — would
     * deadlock. Set its per-thread exception ports to MACH_PORT_NULL
     * so it falls back to system default (kills process). */
    thread_set_exception_ports(mach_thread_self(),
        EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS |
            EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT,
        MACH_PORT_NULL,
        EXCEPTION_DEFAULT, THREAD_STATE_NONE);

    for (;;) {
        yos_mach_exc_request_t req;
        kern_return_t kr = mach_msg(&req.Head,
                                    MACH_RCV_MSG | MACH_RCV_LARGE,
                                    0,
                                    sizeof req,
                                    yos_mach_exc_port,
                                    MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) continue;

        int sig = SIGILL;
        switch (req.exception) {
        case EXC_BAD_INSTRUCTION: sig = SIGILL;  break;
        case EXC_BAD_ACCESS:      sig = SIGSEGV; break;
        case EXC_ARITHMETIC:      sig = SIGFPE;  break;
        case EXC_BREAKPOINT:      sig = SIGTRAP; break;
        default:                  sig = SIGILL;  break;
        }
        yos_mach_dump_crash(sig, req.exception,
                            req.code[0], req.code[1],
                            req.thread.name);
        _exit(128 + sig);
    }
    return NULL;
}

static void yos_mach_install_exc_handler(void)
{
    int dbg = open("/tmp/yos-startup.log",
                   O_WRONLY | O_CREAT | O_APPEND, 0644);
    char b[256];

    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                          MACH_PORT_RIGHT_RECEIVE,
                                          &yos_mach_exc_port);
    if (kr != KERN_SUCCESS) {
        if (dbg >= 0) {
            int n = snprintf(b, sizeof b,
                             "mach_port_allocate failed: %d\n", kr);
            (void)!write(dbg, b, (size_t)n);
            (void)close(dbg);
        }
        return;
    }
    kr = mach_port_insert_right(mach_task_self(), yos_mach_exc_port,
                                yos_mach_exc_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        if (dbg >= 0) {
            int n = snprintf(b, sizeof b,
                             "mach_port_insert_right failed: %d\n", kr);
            (void)!write(dbg, b, (size_t)n);
            (void)close(dbg);
        }
        return;
    }
    kr = task_set_exception_ports(mach_task_self(),
        EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS |
            EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT,
        yos_mach_exc_port,
        EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
        THREAD_STATE_NONE);
    if (kr != KERN_SUCCESS) {
        if (dbg >= 0) {
            int n = snprintf(b, sizeof b,
                             "task_set_exception_ports failed: %d\n", kr);
            (void)!write(dbg, b, (size_t)n);
            (void)close(dbg);
        }
        return;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, yos_mach_exc_thread, NULL);
    pthread_attr_destroy(&attr);

    if (dbg >= 0) {
        int n = snprintf(b, sizeof b,
                         "yos-mach-exc installed pthread_create=%d port=%u\n",
                         rc, (unsigned)yos_mach_exc_port);
        (void)!write(dbg, b, (size_t)n);
        (void)close(dbg);
    }
}
#endif /* __APPLE__ */

static int main_get_asyncify_state(IM3Runtime rt);

/* env.__stack_chk_fail: clang's stack-protector emits a call here
 * when the canary at function entry doesn't match the canary at
 * function exit. nvim's wasm-libc apparently uses memory[0..3] as
 * its canary storage, but address 0 doubles as the wasm-libc thread-
 * struct pointer slot — every libc operation that allocates or
 * re-references the per-thread state writes a new pointer there,
 * which makes any function whose frame straddles such a write trip
 * a false canary smash on exit (nvim's `channel_job_start` is the
 * first long-lived caller after the asyncify-fork dance). With
 * YOS_STACK_CHK_IGNORE=1 we WARN once and return — useful only
 * as a debugging aid to discover what the guest does NEXT after
 * the smash. Default behaviour (false-positive guard): silently
 * succeed, since the next instruction is `unreachable` only when
 * clang knows stack_chk_fail is noreturn — for nvim's wasm32 build
 * that path still ends in an unreachable trap, but at least we
 * don't print the scary backtrace each time. */
static m3ApiRawFunction(m3_yos_stack_chk_fail)
{
    static int warned = 0;
    if (getenv("YOS_STACK_CHK_IGNORE")) {
        if (!warned) {
            fprintf(stderr,
                "yos: __stack_chk_fail (IGNORED — last bridge: %s)\n",
                yos_brg_last_call ? yos_brg_last_call : "(none)");
            warned = 1;
        }
        m3ApiSuccess();
    }
    fprintf(stderr,
        "yos: __stack_chk_fail() — guest stack canary corrupted\n"
        "yos: last bridge before trap: %s\n",
        yos_brg_last_call ? yos_brg_last_call : "(none)");
    {
        struct yos_exec_ctx *ctx =
            (struct yos_exec_ctx *)m3_GetUserData(runtime);
        if (ctx) {
            uint32_t mem_size = 0;
            ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
            ctx->memory_size = mem_size;
        }
        yos_brg_dump_ring(stderr, 256, ctx);
    }
    fflush(stderr);
    m3ApiTrap("__stack_chk_fail");
}

/* env.abort: nvim/libuv calls abort() on lots of unrecoverable paths
 * without a diagnostic message. Print a wasm backtrace so we see WHICH
 * function decided to give up before the host process dies. */
static m3ApiRawFunction(m3_yos_abort)
{
    fprintf(stderr, "yos: abort() called from wasm — last bridge: %s — backtrace:\n",
            yos_brg_last_call);
    fflush(stderr);
    IM3BacktraceInfo bt = m3_GetBacktrace(runtime);
    if (!bt) {
        fprintf(stderr, "yos:   (m3_GetBacktrace returned NULL)\n");
    } else if (!bt->frames) {
        fprintf(stderr, "yos:   (bt->frames is NULL; lastFrame=%p)\n",
                (void *)bt->lastFrame);
    } else {
        IM3BacktraceFrame f = bt->frames;
        int i = 0;
        while (f && i < 64) {
            const char *fn = f->function ? m3_GetFunctionName(f->function) : "?";
            fprintf(stderr, "yos:   #%d %s\n", i++, fn);
            f = f->next;
        }
    }
    fflush(stderr);
    /* Use m3ApiTrap so the runtime unwinds + reports cleanly instead
     * of host abort() which kills any remaining diagnostic. */
    m3ApiTrap("wasm called abort()");
}

/* env.__assert: FreeBSD's assert backend.
 *   void __assert(const char *func, const char *file, int line,
 *                 const char *expr);
 * The FreeBSD <assert.h> macro lowers `assert(e)` to a call here on
 * failure. Diagnose loudly so a stalled wasm program tells us WHICH
 * assertion fired, then abort the host process. */
static m3ApiRawFunction(m3_yos_assert)
{
    m3ApiGetArg(uint32_t, func_off);
    m3ApiGetArg(uint32_t, file_off);
    m3ApiGetArg(int32_t,  line);
    m3ApiGetArg(uint32_t, expr_off);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    const char *func = (func_off && func_off < mem_size)
                     ? (const char *)(ctx->memory + func_off) : "?";
    const char *file = (file_off && file_off < mem_size)
                     ? (const char *)(ctx->memory + file_off) : "?";
    const char *expr = (expr_off && expr_off < mem_size)
                     ? (const char *)(ctx->memory + expr_off) : "?";

    fprintf(stderr, "yos: ASSERT %s:%d in %s: %s\n",
            file, line, func, expr);
    m3ApiTrap("__assert");
}

/* stub for unresolved imports */
static m3ApiRawFunction(m3_unresolved_stub)
{
    IM3Function fn = _ctx->function;
    fprintf(stderr, "yos: unresolved import %s.%s\n",
            fn->import.moduleUtf8, fn->import.fieldUtf8);
    m3ApiTrap("unresolved import");
}

/* FreeBSD libc-internal alias for mktemp(). zsh-wasm imports
 * `_mktemp` directly; route it to yos_mktemp (same semantics, just
 * the underscored variant FreeBSD libc uses internally to bypass
 * user interposers). Bound from below right after
 * yos_brg_link_imports. */
extern uint32_t yos_mktemp(struct yos_exec_ctx *ctx, uint32_t templ);
static m3ApiRawFunction(m3_yos_mktemp_alias)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, templ);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    m3ApiReturn(yos_mktemp(ctx, templ));
}

/* ----------------------------------------------------------------------------
 * setjmp / longjmp via asyncify
 *
 * Same trick fork uses. setjmp's first call kicks off an asyncify unwind so
 * the call stack state is captured into a private buffer (sj_asyncify_ptr).
 * The pump in the main exec loop sees `setjmp_pending`, stops the unwind,
 * starts a rewind from the same buffer, and re-calls _start. The rewind
 * walks back down to setjmp, which detects ASYNCIFY_REWINDING and returns
 * (with value 0 — first-time semantics).
 *
 * longjmp does the analogous unwind into a discard buffer and sets
 * `longjmp_pending` + `longjmp_value`. The pump rewinds from the SAVED
 * setjmp buffer (sj_asyncify_ptr), and m3_setjmp's REWINDING path returns
 * `longjmp_value` to the original setjmp caller.
 *
 * Limitations: only the most recent live setjmp is supported (one buffer per
 * ctx). Nested setjmps would need a stack of buffers; ash and our libc-
 * internal users only set up one at a time.
 * --------------------------------------------------------------------------*/

/* Asyncify save area for setjmp/longjmp. Bigger than fork's because the
 * captured stack at setjmp/longjmp time can include musl/libc frames that
 * don't appear in the simpler fork() path. */
#define SJ_BUF_SIZE  (64 * 1024)

static void main_call_asyncify(IM3Runtime rt, const char *name, uint32_t arg)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, name) == NULL) {
        if (arg != (uint32_t)-1) m3_CallV(f, arg);
        else                     m3_CallV(f);
    }
}

static int main_get_asyncify_state(IM3Runtime rt)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, "asyncify_get_state")) return -1;
    m3_CallV(f);
    int32_t state;
    m3_GetResultsV(f, &state);
    return state;
}

static void main_init_asyncify_buf(struct yos_exec_ctx *ctx, uint32_t buf_ptr)
{
    uint32_t *buf = (uint32_t *)(ctx->memory + buf_ptr);
    buf[0] = buf_ptr + 8;
    buf[1] = buf_ptr + SJ_BUF_SIZE;
}

/* Forward decls — definitions follow m3_setjmp/m3_longjmp. */
static int  sj_find_slot (struct yos_exec_ctx *ctx, uint32_t key);
static int  sj_alloc_slot(struct yos_exec_ctx *ctx, uint32_t jmp_buf_ptr,
                          uint32_t mem_size);
static void sj_reap_dead (struct yos_exec_ctx *ctx);

m3ApiRawFunction(m3_setjmp)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, jmp_buf_ptr);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    /* refresh memory pointer in case it grew since last call */
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    /* Rewind path. Three cases:
     *
     * (1) Longjmp pending, OUR jmp_buf is its target — actual landing.
     *     Stop the rewind, return val, free the slot.
     *
     * (2) Longjmp pending, NOT for us — we're an outer frame on the
     *     rewind path with a setjmp call that's on the way to the
     *     matching setjmp. Return 0 (the original first-call value) so
     *     the replay continues past us. Do NOT start a new unwind.
     *     (Earlier version that did is what spun test_setjmp #4 forever.)
     *
     * (3) No longjmp pending, asyncify REWINDING — first-call
     *     round-trip from yos_setjmp_pump for THIS slot. Stop, return 0.
     *     Slot stays live until matched longjmp / scope exit. */
    int astate = main_get_asyncify_state(runtime);
    if (ctx->longjmp_pending) {
        if (ctx->longjmp_target == jmp_buf_ptr) {
            int32_t val = ctx->longjmp_value;
            ctx->longjmp_pending = 0;
            ctx->longjmp_target  = 0;
            if (astate == ASYNCIFY_REWINDING)
                main_call_asyncify(runtime, "asyncify_stop_rewind", -1);
            /* Do NOT free the slot. C semantics: a setjmp may be the
             * target of an unbounded number of longjmps as long as its
             * owning function hasn't returned. Lua's pcall, ash's
             * commandloop both rely on this. The slot is recycled when
             * the same jmp_buf address gets a fresh setjmp call (treated
             * as "first call" again because longjmp_pending is clear). */
            ydebug("setjmp(%u): post-longjmp landing, returning %d\n",
                   jmp_buf_ptr, val);
            m3ApiReturn(val);
        }
        ydebug("setjmp(%u): replay pass-through (longjmp target=%u)\n",
               jmp_buf_ptr, ctx->longjmp_target);
        m3ApiReturn(0);
    }
    if (astate == ASYNCIFY_REWINDING) {
        main_call_asyncify(runtime, "asyncify_stop_rewind", -1);
        ydebug("setjmp(%u): first-call rewind complete, returning 0\n",
               jmp_buf_ptr);
        m3ApiReturn(0);
    }

    /* First call. Allocate (or reuse) a slot — one asyncify buffer per
     * live setjmp so an inner setjmp can't trample the outer's saved
     * state. Kick off the unwind into THIS slot's buffer. */
    int slot = sj_find_slot(ctx, jmp_buf_ptr);
    if (slot < 0) slot = sj_alloc_slot(ctx, jmp_buf_ptr, mem_size);
    if (slot < 0) {
        /* Try harvesting slots whose owning frame has unwound off the
         * shadow stack. Only kicks in once the table is full so the
         * common case stays cheap. */
        sj_reap_dead(ctx);
        slot = sj_alloc_slot(ctx, jmp_buf_ptr, mem_size);
    }
    if (slot < 0) {
        fprintf(stderr,
                "yos: setjmp slot table exhausted (%zu live setjmps)\n",
                sizeof(ctx->sj_slots)/sizeof(ctx->sj_slots[0]));
        m3ApiReturn(-1);
    }
    main_init_asyncify_buf(ctx, ctx->sj_slots[slot].asyncify_buf);

    ctx->setjmp_pending      = 1;
    ctx->setjmp_pending_slot = slot;
    ydebug("setjmp(%u): starting unwind, slot=%d buf=%u\n",
           jmp_buf_ptr, slot, ctx->sj_slots[slot].asyncify_buf);
    main_call_asyncify(runtime, "asyncify_start_unwind",
                       ctx->sj_slots[slot].asyncify_buf);

    m3ApiReturn(0);  /* placeholder; the real return comes after the rewind */
}

/* Slot allocator + lookup helpers. Defined out-of-line so the unit-test
 * build of yos can be inspected without macros. */
static int sj_find_slot(struct yos_exec_ctx *ctx, uint32_t key)
{
    if (key == 0) return -1;  /* 0 means "unused slot"; never legitimately match */
    for (size_t i = 0; i < sizeof(ctx->sj_slots)/sizeof(ctx->sj_slots[0]); i++)
        if (ctx->sj_slots[i].jmp_buf_ptr == key) return (int)i;
    return -1;
}

/* Read the wasm shadow stack pointer. Returns 0 if the global isn't
 * exported; callers must treat 0 as "unknown — don't recycle".  */
static uint32_t sj_get_stack_pointer(struct yos_exec_ctx *ctx)
{
    IM3Module mod = (IM3Module)ctx->module;
    if (!mod) return 0;
    IM3Global g = m3_FindGlobal(mod, "__stack_pointer");
    if (!g) return 0;
    M3TaggedValue tv = { 0 };
    if (m3_GetGlobal(g, &tv) || tv.type != c_m3Type_i32) return 0;
    return (uint32_t)tv.value.i32;
}

/* Reclaim slots whose jmp_buf lives in the popped portion of the wasm
 * shadow stack. wasm32 musl's stack grows DOWN, so a jmp_buf at addr X
 * is alive only while SP ≤ X. Once the owning function returns the
 * stack pointer climbs back above X and the jmp_buf address is dead;
 * a future longjmp targeting it would be UB anyway (C18 §7.13.2.1¶2).
 * Without this sweep, Lua's pcall pattern (each pcall registers a
 * fresh sigsetjmp at a deeper stack frame) exhausts the slot table
 * after a few hundred pcalls. */
static void sj_reap_dead(struct yos_exec_ctx *ctx)
{
    uint32_t sp = sj_get_stack_pointer(ctx);
    if (!sp) return;
    const size_t N = sizeof(ctx->sj_slots) / sizeof(ctx->sj_slots[0]);
    for (size_t i = 0; i < N; i++) {
        uint32_t jb = ctx->sj_slots[i].jmp_buf_ptr;
        if (jb == 0) continue;
        if (jb < sp) {
            /* Owning frame has returned — slot is dead. */
            ctx->sj_slots[i].jmp_buf_ptr = 0;
            free(ctx->sj_slots[i].save_data);
            ctx->sj_slots[i].save_data = NULL;
            ctx->sj_slots[i].save_size = 0;
        }
    }
}

static int sj_alloc_slot(struct yos_exec_ctx *ctx, uint32_t jmp_buf_ptr,
                          uint32_t mem_size)
{
    const size_t N = sizeof(ctx->sj_slots) / sizeof(ctx->sj_slots[0]);
    for (size_t i = 0; i < N; i++) {
        if (ctx->sj_slots[i].jmp_buf_ptr == 0) {
            ctx->sj_slots[i].jmp_buf_ptr = jmp_buf_ptr;
            /* Layout above the discard buffer (mem_size - 3*SJ_BUF_SIZE),
             * one buffer per slot moving DOWN. */
            ctx->sj_slots[i].asyncify_buf =
                mem_size - (uint32_t)((4 + i) * SJ_BUF_SIZE);
            return (int)i;
        }
    }
    return -1;
}

m3ApiRawFunction(m3_longjmp)
{
    m3ApiGetArg(uint32_t, jmp_buf_ptr);
    m3ApiGetArg(int32_t,  val);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    /* Reject longjmps to a buffer no setjmp has registered. */
    int slot = sj_find_slot(ctx, jmp_buf_ptr);
    if (slot < 0) {
        ydebug("longjmp(%u) without matching setjmp, exiting %d\n",
               jmp_buf_ptr, val);
        exit(val ? val : 1);
    }

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    if (ctx->sj_discard_ptr == 0)
        ctx->sj_discard_ptr = mem_size - 3 * SJ_BUF_SIZE;
    main_init_asyncify_buf(ctx, ctx->sj_discard_ptr);

    ctx->longjmp_pending = 1;
    /* C standard: longjmp(jb, 0) -> setjmp returns 1. Callers must be
     * able to distinguish first-call (0) from landing. */
    ctx->longjmp_value  = (val == 0) ? 1 : val;
    ctx->longjmp_target = jmp_buf_ptr;
    ydebug("longjmp(%u, %d): unwinding to discard=%u\n",
           jmp_buf_ptr, val, ctx->sj_discard_ptr);
    main_call_asyncify(runtime, "asyncify_start_unwind", ctx->sj_discard_ptr);

    m3ApiSuccess();
}

/* Pump for setjmp/longjmp. Drives the asyncify unwind→rewind round-trip.
 *
 * Asyncify's rewind CONSUMES the save buffer as it replays frames, so we
 * keep a host-side snapshot per slot. setjmp_pending means "a slot's
 * first-call unwind is in flight: snapshot its buffer, then rewind".
 * longjmp_pending means "kick off rewind from the matching slot's
 * (host-side) snapshot" — the current wasm-side buffer is from longjmp's
 * own unwind into sj_discard_ptr and is discarded.
 *
 * Loops while another setjmp/longjmp fires during the rewound run
 * (ash registers a fresh sigsetjmp every command-loop iteration).
 *
 * Non-static so the fork-thread exec loop in yos-proc.c can drive the
 * child runtime's setjmp/longjmp too — without that, the child's first
 * setjmp unwinds out of _start and the runtime treats it as a clean
 * exit. */
void yos_setjmp_pump(struct yos_exec_ctx *ctx)
{
    IM3Runtime rt = (IM3Runtime)ctx->runtime;
    while (ctx->setjmp_pending || ctx->longjmp_pending) {
        main_call_asyncify(rt, "asyncify_stop_unwind", -1);

        uint32_t mem_size = 0;
        ctx->memory = m3_GetMemory(rt, &mem_size, 0);
        ctx->memory_size = mem_size;

        int slot;
        if (ctx->setjmp_pending) {
            slot = ctx->setjmp_pending_slot;
            /* setjmp's unwind freshly populated the slot's buffer.
             * Snapshot it so future longjmps can replay. Detect
             * overflow up front — quieter than the OOB trap that the
             * rewind would otherwise produce when its bookkeeping
             * pointer points past the buffer. */
            struct yos_sj_slot *s = &ctx->sj_slots[slot];
            uint32_t *hdr = (uint32_t *)(ctx->memory + s->asyncify_buf);
            uint32_t used = hdr[0] - (s->asyncify_buf + 8);
            uint32_t cap  = hdr[1] - (s->asyncify_buf + 8);
            if (used > cap) {
                fprintf(stderr,
                        "yos: asyncify save overflowed slot=%d used=%u cap=%u\n",
                        slot, used, cap);
            }
            if (!s->save_data) {
                s->save_data = malloc(SJ_BUF_SIZE);
                s->save_size = SJ_BUF_SIZE;
            }
            if (s->save_data)
                memcpy(s->save_data, ctx->memory + s->asyncify_buf, SJ_BUF_SIZE);
        } else {
            /* longjmp landed on the pump. Rewind from the TARGET slot's
             * host-side snapshot back into its wasm buffer (asyncify's
             * own start_rewind will consume the wasm buffer as it
             * replays). */
            slot = sj_find_slot(ctx, ctx->longjmp_target);
            if (slot < 0) {
                fprintf(stderr,
                        "yos: pump cannot find slot for longjmp target %u\n",
                        ctx->longjmp_target);
                ctx->longjmp_pending = 0;
                ctx->longjmp_target  = 0;
                return;
            }
            struct yos_sj_slot *s = &ctx->sj_slots[slot];
            if (s->save_data)
                memcpy(ctx->memory + s->asyncify_buf, s->save_data, SJ_BUF_SIZE);
        }

        main_call_asyncify(rt, "asyncify_start_rewind",
                           ctx->sj_slots[slot].asyncify_buf);
        ctx->setjmp_pending      = 0;
        ctx->setjmp_pending_slot = -1;
        /* longjmp_pending is cleared by m3_setjmp's matching landing. */

        IM3Function start;
        if (m3_FindFunction(&start, rt, "_start") == NULL) {
            M3Result pr = m3_CallV(start);
            if (pr) {
                /* Inner _start trapped — propagate by clearing pending
                 * flags and stashing the trap so the caller sees it.
                 * Without this, a wasm crash inside the rewound _start
                 * would silently fall through as if the pump finished
                 * cleanly. */
                ctx->setjmp_pending = 0;
                ctx->longjmp_pending = 0;
                ctx->pump_trap = pr;
                return;
            }
        }
    }
}

/* m3_syscall_cp_asm + m3_yos_syscall removed — see CLAUDE.md non-
 * negotiable #5. Memory-pointer refresh after `memory.grow` now happens
 * in each generated bridge wrapper instead (TODO: extend bridge.py to
 * emit the refresh; for now bridges run after the initial GetMemory
 * snapshot in main()). */

/* __yos_argc() - return command line argument count */
m3ApiRawFunction(m3_yos_argc)
{
    m3ApiReturnType(int32_t);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    ydebug("m3_yos_argc: ctx=%p, runtime=%p\n", (void*)ctx, (void*)runtime);
    if (ctx) {
        ydebug("m3_yos_argc: ctx->argc=%d, ctx->argv=%p\n", ctx->argc, (void*)ctx->argv);
    } else {
        ydebug("m3_yos_argc: ctx is NULL!\n");
    }
    m3ApiReturn(ctx->argc);
}

/* __yos_env_reload() — test harness hook: dump per-process env and
 * re-import from host environ. See yos_env_reload in impl/env.c. */
extern void yos_env_reload(struct yos_exec_ctx *ctx);
m3ApiRawFunction(m3_yos_env_reload)
{
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;
    yos_env_reload(ctx);
    m3ApiSuccess();
}

/* __yos_argv_setup(char **argv) - copy argv strings to wasm memory */
m3ApiRawFunction(m3_yos_argv_setup)
{
    m3ApiGetArg(uint32_t, argv_ptr);  /* wasm pointer to char*[] */

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    ydebug("argv_setup: argc=%d, argv[0]=%s\n", ctx->argc, ctx->argv[0]);

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    ydebug("argv_setup: argv_ptr=0x%x mem_size=0x%x heap_end=0x%x\n",
           argv_ptr, mem_size, ctx->heap_end);
    if (argv_ptr + 4u * (uint32_t)(ctx->argc + 1) > mem_size) {
        fprintf(stderr,
                "yos: argv_setup: argv_ptr out of range; trapping\n");
        m3ApiTrap("argv_setup: bad argv_ptr");
    }

    /* allocate space for strings at end of current heap. heap_end is
     * normally `__heap_base` from the wasm-ld output; if it's still
     * zero (we never wrote it after load), park strings RIGHT AFTER
     * the argv array — the guest only reads strings via the
     * pointers we write, so they don't have to live at the
     * canonical heap location. */
    uint32_t str_ptr = ctx->heap_end;
    if (str_ptr == 0) {
        str_ptr = (argv_ptr + 4u * ((uint32_t)ctx->argc + 1) + 15u) & ~15u;
    }

    uint32_t *argv_arr = (uint32_t *)(ctx->memory + argv_ptr);

    for (int i = 0; i < ctx->argc; i++) {
        size_t len = strlen(ctx->argv[i]) + 1;
        if (str_ptr + len > ctx->memory_size) {
            fprintf(stderr, "yos: out of memory for argv\n");
            m3ApiTrap("out of memory");
        }
        memcpy(ctx->memory + str_ptr, ctx->argv[i], len);
        argv_arr[i] = str_ptr;
        str_ptr += len;
    }
    argv_arr[ctx->argc] = 0;  /* NULL terminate */

    ctx->heap_end = (str_ptr + 15) & ~15;  /* align to 16 */

    m3ApiSuccess();
}

/* __yos_envc() — number of environment variables to pass to wasm.
 * crt1 calls this before __yos_envp_setup so it can size the env array. */
m3ApiRawFunction(m3_yos_envc)
{
    m3ApiReturnType(int32_t);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    int n = ctx ? ctx->envc : 0;
    ydebug("envc -> %d\n", n);
    m3ApiReturn(n);
}

/* __yos_envp_setup(char **envp) — copy environment strings into wasm
 * memory and fill the wasm-side envp array (NULL-terminated). Mirrors
 * argv_setup's bump allocator. Without this, getenv("PATH") in the
 * wasm always returns NULL and shells fall back to a baked-in default
 * PATH that doesn't match what the user asked for on the host. */
m3ApiRawFunction(m3_yos_envp_setup)
{
    m3ApiGetArg(uint32_t, envp_ptr);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    uint32_t str_ptr = ctx->heap_end;
    uint32_t *envp_arr = (uint32_t *)(ctx->memory + envp_ptr);

    int n = ctx->envc;
    ydebug("envp_setup: envc=%d heap_end=0x%x\n", n, ctx->heap_end);
    for (int i = 0; i < n; i++) {
        const char *s = ctx->envp[i];
        size_t len = strlen(s) + 1;
        if (str_ptr + len > ctx->memory_size) {
            fprintf(stderr, "yos: out of memory for envp\n");
            m3ApiTrap("out of memory");
        }
        memcpy(ctx->memory + str_ptr, s, len);
        envp_arr[i] = str_ptr;
        str_ptr += len;
    }
    envp_arr[n] = 0;
    ydebug("envp_setup done, heap_end now 0x%x\n", (str_ptr + 15) & ~15);

    ctx->heap_end = (str_ptr + 15) & ~15;
    m3ApiSuccess();
}

/* Link all YOS imports to a module - used by both main and fork child */
void yos_link_imports(IM3Module module, struct yos_exec_ctx *ctx)
{
    /* env.__yos_syscall + env.__syscall_cp_asm bindings removed —
     * see CLAUDE.md non-negotiable #5. The new convention: each libc
     * function is its own env import, resolved by yos_brg_link_imports
     * below. */
    m3_LinkRawFunctionEx(module, "env", "__yos_argc",
                         "i()", m3_yos_argc, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_argv_setup",
                         "v(i)", m3_yos_argv_setup, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_envc",
                         "i()", m3_yos_envc, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_envp_setup",
                         "v(i)", m3_yos_envp_setup, ctx);
    /* __yos_env_reload — test-only hook: drops the per-process env
     * table and re-pulls from host environ. The freebsd-libc test
     * harness calls this between ATF test cases to mimic atf-run's
     * fork-per-test isolation, so a previous test's clearenv() can't
     * starve a later test of e.g. PWD. */
    m3_LinkRawFunctionEx(module, "env", "__yos_env_reload",
                         "v()", m3_yos_env_reload, ctx);
    m3_LinkRawFunction(module, "env", "__error", "i()", m3_yos_error);
    m3_LinkRawFunction(module, "env", "__assert", "v(iiii)", m3_yos_assert);
    m3_LinkRawFunction(module, "env", "abort", "v()", m3_yos_abort);
    m3_LinkRawFunction(module, "env", "__stack_chk_fail", "v()", m3_yos_stack_chk_fail);
    /* FreeBSD signal helpers — sigset_t layout differs between FreeBSD
     * (16 bytes) and Linux (128 bytes), so direct passthrough corrupts
     * memory. These tiny shims operate on the FreeBSD-shape mask in the
     * guest's linear memory.
     *
     * pthread_sigmask / sigprocmask: no-op success (we don't actually
     * mask anything host-side). The guest sees its sigset_t bits if it
     * passes one in, but we don't apply them. nvim's main path doesn't
     * rely on real signal masking. */
    extern void yos_signal_link(IM3Module mod);
    yos_signal_link(module);
    extern void yos_callback_link(IM3Module mod);
    yos_callback_link(module);
    extern void yos_sysctl_link(IM3Module mod);
    yos_sysctl_link(module);
    extern void yos_strto_link(IM3Module mod);
    yos_strto_link(module);
    extern void yos_kqueue_link(IM3Module mod);
    yos_kqueue_link(module);
    m3_LinkRawFunction(module, "env", "setjmp", "i(i)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "longjmp", "v(ii)", m3_longjmp);
    m3_LinkRawFunction(module, "env", "_setjmp", "i(i)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "_longjmp", "v(ii)", m3_longjmp);
    m3_LinkRawFunction(module, "env", "sigsetjmp", "i(ii)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "siglongjmp", "v(ii)", m3_longjmp);

    /* L1 pthread imports — bind every yos_pthread_* the musl wasm32 shim
     * (src/musl/arch/wasm32/thread/yos_pthread_shim.c) calls into.  Lazy-
     * allocate the host on first use; main/proc subsystems share it via
     * g_runtime->pthread_host. */
    if (ctx && ctx->rt) {
        struct yos_pthread_host *ph = (struct yos_pthread_host *)ctx->rt->pthread_host;
        if (!ph) {
            /* Pass the master runtime + the wasm bytes so worker_main
             * can m3_NewSiblingRuntime + re-parse the same module on
             * each thread. Without these, clone() spawns a thread
             * that immediately fails to ParseModule(NULL) and bails
             * silently. The wasm-bytes pointer must outlive every
             * thread; ctx->wasm_bytes is kept around for fork too. */
            IM3Runtime master = (IM3Runtime)ctx->runtime;
            IM3Environment env = master ? master->environment : NULL;
            ph = yos_pthread_host_create (env, master,
                                          ctx->wasm_bytes,
                                          (uint32_t)ctx->wasm_bytes_size,
                                          /*per_thread_stack=*/64 * 1024,
                                          /*tls_pool_base=*/0,
                                          /*tls_arena_size=*/0);
            ctx->rt->pthread_host = ph;
        }
        if (ph) {
            M3Result lerr = yos_pthread_host_link (ph, module);
            if (lerr) {
                fprintf(stderr, "yos: pthread_host_link failed: %s\n", lerr);
            }
        }
    }

    /* Soft-f128 builtins. clang's wasm32 ABI makes `long double` =
     * binary128, and musl's printf/strtod path uses long double freely.
     * We implement the compiler-rt-style __*tf* helpers in the host. */
    extern void yos_f128_link (IM3Module mod);
    yos_f128_link (module);

    /* clang renames user main(int, char**) to __main_argc_argv and
     * emits a wrapper main(void) that's exported. Our crt1's call to
     * main(argc, argv) therefore becomes env.__main_argc_argv. Bind
     * it to a host trampoline that calls the wasm-module's real main. */
    m3_LinkRawFunction(module, "env", "__main_argc_argv", "i(ii)",
                       m3_main_argc_argv);

    /* Variadic printf family. */
    m3_LinkRawFunction(module, "env", "printf",    "i(ii)",   m3_printf);
    m3_LinkRawFunction(module, "env", "fprintf",   "i(iii)",  m3_fprintf);
    m3_LinkRawFunction(module, "env", "sprintf",   "i(iii)",  m3_sprintf);
    m3_LinkRawFunction(module, "env", "snprintf",  "i(iiii)", m3_snprintf);
    m3_LinkRawFunction(module, "env", "vprintf",   "i(ii)",   m3_vprintf);
    m3_LinkRawFunction(module, "env", "vfprintf",  "i(iii)",  m3_vfprintf);
    m3_LinkRawFunction(module, "env", "vsprintf",  "i(iii)",  m3_vsprintf);
    m3_LinkRawFunction(module, "env", "vsnprintf", "i(iiii)", m3_vsnprintf);

    /* Per-ctx libc-globals isolation (build-tools/libbridge/policies/libc.yaml).
     *
     * These bind BEFORE yos_brg_link_imports so the per-ctx versions
     * win over auto-bridges that would otherwise call host libc and
     * mutate the cross-guest shared globals. Each is documented in
     * the policy as `bridged_per_ctx via: impl/<file>.c`. */
    extern void yos_libc_globals_link(IM3Module mod);
    yos_libc_globals_link(module);

    /* exec family — must bind BEFORE yos_brg_link_imports so our trapping
     * versions win over the auto-generated non-trapping ones. */
    m3_LinkRawFunction(module, "env", "execvp",  "i(ii)",  m3_execvp);
    m3_LinkRawFunction(module, "env", "execv",   "i(ii)",  m3_execv);
    m3_LinkRawFunction(module, "env", "execve",  "i(iii)", m3_execve);
    m3_LinkRawFunction(module, "env", "execvpe", "i(iii)", m3_execvpe);

    /* FreeBSD userland fns the bridge auto-stubs to NULL because their
     * signatures (returning host-allocated char *, writing through
     * char **) don't fit the mechanical pointer-translation pattern.
     * Without these, the second-batch tool ports (cut, sort, grep,
     * sed, awk, du, df, …) trap on first strdup / asprintf / strerror.
     * Bound BEFORE yos_brg_link_imports so we win the race against
     * its NULL-returning auto-stub. */
    extern void yos_freebsd_userland_link(IM3Module mod);
    yos_freebsd_userland_link(module);

    /* libpython 3.12 — env.Py_Initialize / Py_Finalize / PyRun_SimpleString.
     * The yos host links against libpython3.12.so; the wasm guest is a
     * tiny driver that imports these names. See impl/libpython.c. */
    extern void yos_libpython_link(IM3Module mod);
    yos_libpython_link(module);

    /* Auto-generated bridges for the FreeBSD-libc-name import surface.
     * For guests that import each libc fn by name (env.write, env.read,
     * env.exit, …) instead of going through __yos_syscall. Bridges
     * tolerate "function not found" so this is harmless for legacy
     * __yos_syscall-using guests too. See CLAUDE.md for the design.
     * Must run BEFORE the wildcard stub below or the stub captures
     * every name first. */
    extern int yos_brg_link_imports (IM3Module mod);
    int brg_rc = yos_brg_link_imports (module);
    if (brg_rc != 0) {
        fprintf(stderr, "yos: yos_brg_link_imports failed (rc=%d) — most "
                        "libc imports will fall through to the unresolved "
                        "stub\n", brg_rc);
    }

    /* syslog (openlog/closelog/syslog) + FreeBSD libutil (login_tty,
     * realhostname_sa). Not in the codegen surface because <syslog.h>
     * / <libutil.h> aren't in api_top_headers (those drag in
     * machine-specific decls that break wasm32 extraction). Bridges
     * are hand-written in impl/syslog_extras.c; bind them AFTER the
     * codegen link pass so the m3 unresolved-import check sees them
     * resolved at load time. telnetd needs all five to load. */
    extern void yos_syslog_extras_link_imports (IM3Module mod);
    yos_syslog_extras_link_imports (module);

    /* FreeBSD-internal libc-private aliases. The public POSIX name
     * (mktemp) gets bridged via yos_brg_link_imports; the underscored
     * variant (_mktemp) is what FreeBSD libc's *.c source calls when
     * it wants to bypass user interposers — same semantics, different
     * symbol. zsh-wasm pulls in _mktemp through one of those internal
     * call paths and would trap on the unresolved-import wildcard
     * without this bind. m3w_mktemp inside yos_bridge.c is `static`,
     * so we wrap yos_mktemp ourselves rather than referencing the
     * generated wrapper. Extend with more aliases as the import scan
     * (tools/scan-imports.sh) surfaces them. */
    m3_LinkRawFunction(module, "env", "_mktemp", "i(i)",
                       m3_yos_mktemp_alias);

    /* link wildcard stub for all remaining unresolved imports */
    m3_LinkRawFunction(module, "env", "*", NULL, m3_unresolved_stub);
}

static uint8_t *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "yos: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "yos: read error: %s\n", path);
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

/* Global runtime - process table shared across all processes */
static struct yos_runtime g_runtime;

/* Forward declarations */
extern struct yos_proc *yos_proc_alloc(struct yos_runtime *rt, int32_t ppid);
extern void yos_fork_pump(struct yos_exec_ctx *ctx);
extern void yos_vfork_pump(struct yos_exec_ctx *ctx);
extern void yos_signal_set_pending(int fbsd_signum);

/* Translate host (Linux) signal numbers to FreeBSD wasm-guest signums.
 * Most match (POSIX-defined values are the same on Linux & FreeBSD)
 * but a handful diverge — track only the ones we forward. */
static int host_signum_to_fbsd(int host_sig)
{
    switch (host_sig) {
        case SIGHUP:   return 1;
        case SIGINT:   return 2;
        case SIGQUIT:  return 3;
        case SIGABRT:  return 6;
        case SIGPIPE:  return 13;
        case SIGTERM:  return 15;
        case SIGCONT:  return 19;  /* FreeBSD SIGCONT */
        case SIGTSTP:  return 18;
        case SIGTTIN:  return 21;
        case SIGTTOU:  return 22;
        case SIGWINCH: return 28;
        default:       return -1;
    }
}

static void host_signal_dispatcher(int host_sig)
{
    int fbsd = host_signum_to_fbsd(host_sig);
    if (fbsd > 0) yos_signal_set_pending(fbsd);
}

static void yos_install_host_signal_handlers(void)
{
    /* No SA_RESTART. We DO want blocking reads/writes to return
     * EINTR when a forwardable signal hits the host, so the guest's
     * recorded wasm handler can fire on the way out and (for zsh)
     * the line-editor sees Ctrl-C immediately rather than waiting
     * for the next keystroke. yos_read pumps the pending bitmask
     * before and after the system call. */
    struct sigaction sa = { .sa_handler = host_signal_dispatcher,
                            .sa_flags   = 0 };
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN,
                   SIGTTOU, SIGWINCH, SIGHUP, SIGPIPE, SIGCONT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

/* Load and prepare a wasm module. Returns 0 on success. */
static int load_wasm_module(struct yos_exec_ctx *ctx, IM3Environment env,
                            const char *path, uint8_t **out_bytes, size_t *out_size)
{
    size_t wasm_size = 0;
    uint8_t *wasm_bytes = load_file(path, &wasm_size);
    if (!wasm_bytes) return -1;

    /* Create new runtime */
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, 0);
    if (!rt) {
        fprintf(stderr, "yos: failed to create wasm3 runtime\n");
        free(wasm_bytes);
        return -1;
    }

    IM3Module module = 0;
    M3Result res = m3_ParseModule(env, &module, wasm_bytes, wasm_size);
    if (res) {
        fprintf(stderr, "yos: parse error: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }

    res = m3_LoadModule(rt, module);
    if (res) {
        fprintf(stderr, "yos: load error: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }

    /* Update context BEFORE linking imports — yos_link_imports() now
     * lazy-creates the L1 pthread host and reads ctx->runtime to get the
     * wasm3 environment. Setting it after the link left env NULL and
     * silently skipped binding the yos_pthread_* imports, which made libc
     * internals trap into the wildcard "unresolved import" stub and kill
     * the shell after a failed exec. */
    ctx->runtime = rt;
    ctx->module = module;
    rt->userdata = ctx;

    /* The pthread_host (lazy-allocated by yos_link_imports below) needs
     * the wasm bytes + size to spawn sibling runtimes for clone()ed
     * threads. Set them BEFORE the link so the host stores the right
     * pointer. */
    ctx->wasm_bytes = wasm_bytes;
    ctx->wasm_bytes_size = wasm_size;

    /* Link imports */
    yos_link_imports(module, ctx);

    /* Grow memory. Default cap is 4096 pages * 64 KiB = 256 MiB —
     * nvim's Lua + module dictionaries easily blow past 16 MiB during
     * startup. BUT every fork's snapshot/restore copies the full
     * committed size, so blindly committing 256 MiB makes fork() cost
     * ~300 ms even for guests that barely use 1 MiB.
     *
     * Pick the smallest size that satisfies the module's contract:
     *   - if the wasm declares --max-memory, cap by that (and never
     *     grow past the module's own declared limit anyway).
     *   - else use 4096 pages as before for nvim's sake.
     * A guest that needs more than its declared initial can still
     * memory.grow() up to maxPages at runtime — wasm3 handles that. */
    extern M3Result ResizeMemory(IM3Runtime, uint32_t);
    uint32_t resize_pages = 4096;
    if (module->memoryInfo.maxPages > 0 &&
        module->memoryInfo.maxPages < resize_pages) {
        resize_pages = module->memoryInfo.maxPages;
    }
    if (module->memoryInfo.initPages > resize_pages) {
        resize_pages = module->memoryInfo.initPages;
    }
    res = ResizeMemory(rt, resize_pages);
    if (res) {
        fprintf(stderr, "yos: memory resize: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }
    ydebug("memory: resized to %u pages (%u MiB) — init=%u max=%u\n",
           resize_pages, resize_pages / 16,
           module->memoryInfo.initPages, module->memoryInfo.maxPages);

    /* Get memory info */
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(rt, &mem_size, 0);
    ctx->memory_size = mem_size;

    /* Heap starts at __heap_base — wasm-ld places it just past the data
     * segment. A hardcoded constant (e.g. 0x50000) corrupts .data for any
     * non-trivial wasm: argv strings get written into static globals, then
     * brk-driven malloc hands out chunks that overlap musl's `mal` struct
     * (oldmalloc bins/binmap), and a later trim()->bin_chunk() reads a
     * trampled bins[i].tail and traps on tail->next. nvim hits this within
     * a few thousand mallocs because its .data ends near 0x10C800.
     *
     * Tiny wasms (kernel unit tests) link without --export-all and don't
     * surface __heap_base. They fit under 0x50000, so fall back there. */
    ctx->heap_end = 0x50000;
    {
        IM3Global g = m3_FindGlobal(module, "__heap_base");
        if (g) {
            M3TaggedValue tv = { 0 };
            M3Result r = m3_GetGlobal(g, &tv);
            if (!r && tv.type == c_m3Type_i32) {
                uint32_t hb = (tv.value.i32 + 15) & ~15u;
                if (hb > ctx->heap_end) ctx->heap_end = hb;
            }
        }
    }

    /* Initialize TLS */
    uint32_t *thread_ptr = (uint32_t *)(ctx->memory + 0);
    *thread_ptr = 0x100;
    memset(ctx->memory + 0x100, 0, 256);

    /* Errno slot for the FreeBSD `errno` macro (= `*__error()`). Sits
     * inside the TLS block we just zeroed at 0x100..0x200 — picked an
     * 8-byte-aligned offset past the self-pointer at 0x100. Bridges
     * write here on error; main.c binds env.__error to a trampoline
     * that returns this offset. */
    ctx->errno_off = 0x108;

    extern void yos_fd_table_init(struct yos_exec_ctx *);
    yos_fd_table_init(ctx);

    *out_bytes = wasm_bytes;
    *out_size = wasm_size;
    return 0;
}

/* Free exec argv */
static void free_exec_argv(struct yos_exec_ctx *ctx)
{
    if (ctx->exec_argv) {
        for (int i = 0; i < ctx->exec_argc; i++) {
            free(ctx->exec_argv[i]);
        }
        free(ctx->exec_argv);
        ctx->exec_argv = NULL;
    }
    ctx->exec_argc = 0;
    ctx->exec_pending = 0;
}

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: yos <program.wasm>\n");
        return 1;
    }

    /* YOS_BRG_TRACE=1 makes every generated bridge log its name. Lets us
     * see exactly what libc function the wasm guest called last when it
     * crashes without a useful host-visible error. */
    if (getenv("YOS_BRG_TRACE")) yos_brg_trace = 1;

    /* Crash diagnostics. The crashing thread on darwin can have a
     * corrupted stack (e.g. wasm3 jumped into garbage) — install a
     * sigaltstack so the handler has somewhere safe to run. Use
     * SA_ONSTACK + SA_SIGINFO to also receive the faulting address. */
    {
        extern void yos_host_crash_handler_si(int, siginfo_t *, void *);
        /* glibc 2.34+ made SIGSTKSZ a sysconf() call (not a constant),
         * so it can't size a static array. 64 KiB is well above
         * MINSIGSTKSZ on every platform we target. */
        static char altstack_buf[64 * 1024];
        stack_t ss = {0};
        ss.ss_sp = altstack_buf;
        ss.ss_size = sizeof altstack_buf;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
        struct sigaction sa = {0};
        sa.sa_sigaction = yos_host_crash_handler_si;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS,  &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGFPE,  &sa, NULL);
        sigaction(SIGTRAP, &sa, NULL);
        /* Make sure these synchronous signals are unblocked on the
         * main thread. Worker threads inherit our mask. */
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGBUS);
        sigaddset(&unblock, SIGSEGV);
        sigaddset(&unblock, SIGILL);
        sigaddset(&unblock, SIGABRT);
        sigaddset(&unblock, SIGFPE);
        sigaddset(&unblock, SIGTRAP);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
    }

#ifdef __APPLE__
    /* Darwin: synchronous CPU faults are routed through Mach exception
     * ports BEFORE BSD signals. Install a Mach handler so we get a
     * register dump on SIGILL/SIGSEGV/SIGFPE — the POSIX handler above
     * never fires for those on darwin. */
    yos_mach_install_exc_handler();
#endif

    /* SIGUSR1 → dump the bridge ring buffer (with tid per call) to
     * /tmp/yos-host-ring.log. Lets us peek at what each thread is
     * doing when nvim hangs without crashing. */
    {
        extern void yos_brg_dump_handler(int);
        struct sigaction sa = {0};
        sa.sa_handler = yos_brg_dump_handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
    }

    /* Install host-side signal forwarders BEFORE any wasm runs.
     *
     * The user's terminal driver sends SIGINT/SIGQUIT/SIGTSTP/SIGWINCH
     * etc. to yos's host process. Default kernel disposition is
     * "terminate" for the first three, which kills yos and every
     * guest along with it. We intercept and forward to the foreground
     * guest's wasm-side handler via impl/sig.c::yos_signal_pump.
     *
     * `yos_install_host_signal_handlers` is defined below in this
     * file. Translate host→FreeBSD signum (Linux SIGINT=2 == FreeBSD;
     * Linux SIGSTOP=19 vs FreeBSD SIGSTOP=17; SIGCHLD: Linux=17
     * FreeBSD=20) when pushing to the pending bitmask. */
    yos_install_host_signal_handlers();

    /* Initialize global runtime */
    memset(&g_runtime, 0, sizeof(g_runtime));
    pthread_mutex_init(&g_runtime.proc_lock, NULL);
    pthread_cond_init(&g_runtime.any_exit_cond, NULL);
    g_runtime.next_pid = 1;
    g_runtime.fg_pgid  = 1; /* init proc owns the tty at startup */
    g_runtime.argc = argc - 1;
    g_runtime.argv = argv + 1;
    /* Pass yos's own environment through to the wasm program. The shell
     * inside busybox / nvim plugins / etc. all read PATH, HOME, TERM,
     * etc. via getenv(); without inheriting they see an empty
     * environment and fall back to defaults that don't match what the
     * user set on the host shell. */
    {
        int ec = 0;
        if (environ) for (char **p = environ; *p; p++) ec++;
        g_runtime.envc = ec;
        g_runtime.envp = environ;
    }

    /* Initialize VFS mount table and mount /proc */
    static struct yos_mount_table mount_table;
    yos_mount_table_init(&mount_table);
    yos_mount_add(&mount_table, "/proc", &yos_procfs_ops);
    g_runtime.mount_table = &mount_table;

    IM3Environment env = m3_NewEnvironment();

    /* Allocate initial process (pid 1, ppid 0) */
    struct yos_proc *proc = yos_proc_alloc(&g_runtime, 0);
    if (!proc) {
        fprintf(stderr, "yos: failed to allocate process\n");
        return 1;
    }
    proc->state = YOS_PROC_RUNNING;
    /* Record the main thread so kill(pid=1, sig) / tkill / tgkill from
     * inside the wasm can resolve the guest pid back to a real host
     * thread via pthread_kill(). Forked procs get this filled in by
     * the fork-thread spawn path. */
    proc->thread = pthread_self();

    /* Initialize process info */
    if (!getcwd(proc->cwd, sizeof(proc->cwd)))
        strcpy(proc->cwd, "/");
    strncpy(proc->exe, argv[1], PATH_MAX - 1);
    const char *slash = strrchr(argv[1], '/');
    strncpy(proc->comm, slash ? slash + 1 : argv[1], sizeof(proc->comm) - 1);
    proc->cmdline = argv + 1;
    proc->cmdline_argc = argc - 1;
    /* Label the trace file for the initial process. */
    yos_ytrace_set_comm(proc->comm);

    /* Set up exec context */
    struct yos_exec_ctx ctx = {0};
    ctx.rt = &g_runtime;
    yos_brg_dump_ctx = &ctx;
    ctx.proc = proc;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;
    ctx.envc = g_runtime.envc;
    ctx.envp = g_runtime.envp;
    pthread_mutex_init(&ctx.mem_lock, NULL);
    strcpy(ctx.cwd, proc->cwd);

    /* Load initial module */
    size_t wasm_size = 0;
    uint8_t *wasm_bytes = NULL;
    if (load_wasm_module(&ctx, env, argv[1], &wasm_bytes, &wasm_size) != 0) {
        return 1;
    }
    ctx.wasm_bytes = wasm_bytes;
    ctx.wasm_bytes_size = wasm_size;

    ydebug("main: ctx=%p, rt=%p\n", (void*)&ctx, ctx.runtime);

    /* Main exec loop - run _start, reload module if exec happens */
    for (;;) {
        IM3Function start_fn;
        M3Result res = m3_FindFunction(&start_fn, ctx.runtime, "_start");
        if (res) {
            fprintf(stderr, "yos: _start not found: %s\n", res);
            return 1;
        }

        res = m3_CallV(start_fn);

        /* Drain pumps until quiescent. Each pump's internal m3_CallV(_start)
         * resumes wasm, which may trigger another pump (e.g. an ash command
         * loop does setjmp → rewind → fork → setjmp ...). Loop until no
         * fork/setjmp/longjmp event is pending. */
        for (;;) {
            int progress = 0;
            if (ctx.fork_pending) { yos_fork_pump(&ctx);  progress = 1; }
            if (ctx.setjmp_pending || ctx.longjmp_pending) {
                yos_setjmp_pump(&ctx);
                progress = 1;
            }
            if (!progress) break;
        }
        /* Surface a trap from inside the pump as the outer res so the
         * "main returned cleanly" path doesn't swallow it. */
        if (ctx.pump_trap) { res = ctx.pump_trap; ctx.pump_trap = NULL; }

        /* Check if exec happened */
        if (!ctx.exec_pending) {
            if (res) {
                ydebug("trap: %s\n", res);
                IM3BacktraceInfo bt = m3_GetBacktrace(ctx.runtime);
                if (bt) {
                    IM3BacktraceFrame f = bt->frames;
                    while (f) {
                        ydebug("  at %s\n", f->function ? m3_GetFunctionName(f->function) : "?");
                        f = f->next;
                    }
                }
            }
            break;
        }

        /* Handle exec - load new module */
        ydebug("exec: loading %s\n", ctx.exec_path);

        m3_FreeRuntime(ctx.runtime);
        free(wasm_bytes);

        ctx.argc = ctx.exec_argc;
        ctx.argv = ctx.exec_argv;
        /* If the wasm passed an envp, install it as the new env. NULL
         * means "no env replacement" — POSIX exec says the new image
         * gets the caller's envp, but inheriting the existing ctx
         * matches what musl does when given environ as envp. */
        if (ctx.exec_envp) {
            ctx.envc = ctx.exec_envc;
            ctx.envp = ctx.exec_envp;
        }

        if (load_wasm_module(&ctx, env, ctx.exec_path, &wasm_bytes, &wasm_size) != 0) {
            fprintf(stderr, "yos: exec failed to load %s\n", ctx.exec_path);
            free_exec_argv(&ctx);
            return 127;
        }
        ctx.wasm_bytes = wasm_bytes;
        ctx.wasm_bytes_size = wasm_size;
        ctx.free_count = 0;

        /* execve(2) replaces the process image — update comm and exe
         * on the yos_proc so /proc/<pid>/{stat,comm,exe} reflect the
         * new program. Without this, `ps` keeps showing the parent's
         * name for every forked-and-exec'd child (e.g. a ps invoked
         * from zsh would show up as "zsh" or empty). */
        if (ctx.proc) {
            const char *slash = strrchr(ctx.exec_path, '/');
            const char *base = slash ? slash + 1 : ctx.exec_path;
            strncpy(ctx.proc->comm, base, sizeof(ctx.proc->comm) - 1);
            ctx.proc->comm[sizeof(ctx.proc->comm) - 1] = '\0';
            strncpy(ctx.proc->exe, ctx.exec_path, sizeof(ctx.proc->exe) - 1);
            ctx.proc->exe[sizeof(ctx.proc->exe) - 1] = '\0';
            /* Re-label the per-thread ytrace file. */
            yos_ytrace_set_comm(ctx.proc->comm);
        }

        ctx.exec_pending = 0;
        ctx.exec_argv = NULL;
        ctx.exec_argc = 0;
        ctx.exec_envp = NULL;
        ctx.exec_envc = 0;
        /* Allocator state lives IN the (just-replaced) linear memory.
         * Reset the wasm-offset bookmarks so the new image's first
         * malloc lazy-inits a fresh heap. */
        ctx.alloc_lo = 0;
        ctx.alloc_hi = 0;
        ctx.alloc_free_head = 0;

        ydebug("exec: loaded %s, argc=%d\n", ctx.exec_path, ctx.argc);
    }

    /* The initial proc (pid 1) has finished. There may still be
     * forked children running in their own pthreads — block here
     * until every non-init proc has reached ZOMBIE before tearing
     * down the host. Event-driven: each proc-exit path broadcasts
     * rt->any_exit_cond, we wake, recount, and either sleep again
     * or fall through to teardown. No polling. */
    {
        pthread_mutex_lock(&g_runtime.proc_lock);
        for (;;) {
            int alive = 0;
            for (int i = 0; i < YOS_MAX_PROCS; i++) {
                struct yos_proc *p = &g_runtime.procs[i];
                if (p->state == YOS_PROC_RUNNING && p->pid != 1) {
                    alive++;
                }
            }
            if (!alive) break;
            pthread_cond_wait(&g_runtime.any_exit_cond,
                              &g_runtime.proc_lock);
        }
        pthread_mutex_unlock(&g_runtime.proc_lock);
    }

    m3_FreeRuntime(ctx.runtime);
    m3_FreeEnvironment(env);
    free(wasm_bytes);
    return 0;
}
