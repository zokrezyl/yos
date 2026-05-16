#include "yos/types.h"
#include "yos/ydebug.h"
#include <stdint.h>
#include <string.h>     /* memset for sigemptyset/sigfillset */
#include <errno.h>
#include <pthread.h>
#include <signal.h>     /* host sigset_t, SIGHUP etc. for pthread_sigmask */
#include <time.h>
#include <unistd.h>     /* usleep */

#include "wasm3.h"
#include "m3_env.h"
#include "errno_helpers.h"  /* yos_remap_errno_h2g for pthread_sigmask */

/* Signal handling stubs - we can't actually deliver signals in WASM,
 * but we return success so programs think they registered handlers. */

/* Recorded signal-handler function-table indices. zsh installs SIGCHLD
 * via sigaction during job-control init; sigsuspend then blocks
 * waiting for the handler to fire and update STAT_DONE. yos doesn't
 * have async signal delivery, so we record the handler here and
 * synthesise a synchronous call from sigsuspend whenever a child of
 * the calling proc has reached ZOMBIE state. SIGCHLD is by far the
 * most common case; track up to 32 entries to cover SIGINT etc. too. */
#define YOS_NSIG 32
static uint32_t g_signal_handlers[YOS_NSIG];

static void record_handler(int signum, uint32_t handler_idx)
{
    if (signum > 0 && signum < YOS_NSIG)
        g_signal_handlers[signum] = handler_idx;
}

/* Async signal forwarding from the host.
 *
 * When the user hits Ctrl-C (or any tty-driver signal — SIGINT,
 * SIGQUIT, SIGTSTP, SIGWINCH, …) the KERNEL delivers it to yos's
 * host process, not to any wasm guest directly. Without explicit
 * handling the kernel default kicks in (SIGINT/QUIT terminate; the
 * whole yos process dies, taking every guest with it).
 *
 * We want the signal forwarded to the foreground guest proc's
 * recorded wasm handler instead, then the host runtime stays alive.
 *
 * Async-safety: signal handlers can't call m3_CallV (it locks
 * pthread mutexes and walks runtime state). Two-stage delivery:
 *
 *   1. Host signal handler (installed by yos_install_host_signal_
 *      handlers in main.c) sets a bit in g_host_pending_signals.
 *   2. Any wasm thread, on its way out of a blocking syscall
 *      (yos_read et al.) calls yos_signal_pump(ctx); the pump
 *      drains the pending set and invokes the foreground guest's
 *      wasm-side handler synchronously. */
static volatile uint32_t g_host_pending_signals;  /* atomic bitmask, FreeBSD signums */

void yos_signal_set_pending(int fbsd_signum)
{
    if (fbsd_signum <= 0 || fbsd_signum >= YOS_NSIG) return;
    __atomic_or_fetch(&g_host_pending_signals, 1u << fbsd_signum,
                      __ATOMIC_RELEASE);
}

/* Invoke the wasm-side handler for `signum` (if any). Function table
 * index was recorded by sigaction; we look it up in the same way
 * impl/callback.c does for qsort comparators. Bails silently on
 * any error — the caller is sigsuspend, and we still want to return
 * EINTR so the wait loop sees forward progress. */
static void invoke_signal_handler(struct yos_exec_ctx *ctx, int signum)
{
    if (signum <= 0 || signum >= YOS_NSIG) return;
    uint32_t idx = g_signal_handlers[signum];
    ydebug("invoke_signal_handler: sig=%d idx=%u\n", signum, idx);
    if (!idx) return;
    IM3Runtime rt = (IM3Runtime)ctx->runtime;
    if (!rt) return;
    IM3Module mod = rt->modules;
    if (!mod || idx >= mod->table0Size) {
        ydebug("invoke_signal_handler: idx %u out of table size %u\n",
               idx, mod ? mod->table0Size : 0);
        return;
    }
    IM3Function fn = mod->table0[idx];
    if (!fn) { ydebug("invoke_signal_handler: table[%u] is NULL\n", idx); return; }
    if (!fn->compiled && CompileFunction(fn) != NULL) {
        ydebug("invoke_signal_handler: CompileFunction failed\n");
        return;
    }
    /* sa_handler is `void (*)(int)`; pass the signum. */
    M3Result r = m3_CallV(fn, (uint32_t)signum);
    ydebug("invoke_signal_handler: m3_CallV returned %s\n", r ? r : "OK");
}

/* Drain the host-side pending-signal bitmask, dispatching each set
 * bit to this proc's wasm handler. Called from yos_read (and other
 * places where a guest thread is naturally about to re-enter wasm)
 * to deliver host-originated SIGINT/SIGQUIT/SIGWINCH/SIGTSTP. The
 * foreground-vs-background distinction is intentionally loose here:
 * every wasm thread that reaches the pump drains the same global
 * set, so the FIRST one to drain wins. That's what we want for
 * Ctrl-C at zsh's prompt — zsh is the one blocked in read() and
 * gets the signal. Multi-process delivery to non-foreground procs
 * would need per-proc pending sets keyed on `proc->pgid ==
 * rt->fg_pgid`; ignore for now. */
void yos_signal_pump(struct yos_exec_ctx *ctx)
{
    if (!ctx) return;
    uint32_t pending = __atomic_exchange_n(&g_host_pending_signals, 0,
                                            __ATOMIC_ACQ_REL);
    if (!pending) return;
    for (int s = 0; s < YOS_NSIG; s++) {
        if (pending & (1u << s))
            invoke_signal_handler(ctx, s);
    }
}

int32_t yos_sig_rt_sigaction(struct yos_exec_ctx *ctx, int32_t signum,
                              uint32_t act, uint32_t oldact, uint32_t sigsetsize)
{
    (void)ctx;
    ydebug("rt_sigaction(sig=%d, act=0x%x, oldact=0x%x, size=%u) -> 0 (stub)\n",
           signum, act, oldact, sigsetsize);
    /* Record the wasm-side handler so sigsuspend can invoke it
     * synchronously when the corresponding proc-table event fires
     * (child zombie → SIGCHLD). FreeBSD i386 struct sigaction has
     * sa_handler (or sa_sigaction in the union) at offset 0; the
     * value is a function-table index, NOT a host pointer. */
    if (act && ctx && ctx->memory && act + 4 <= ctx->memory_size) {
        uint32_t handler = *(uint32_t *)(ctx->memory + act);
        record_handler(signum, handler);
    }
    /* If oldact is provided, we should write the old action there.
     * For now, just zero it out if provided. */
    if (oldact && ctx) {
        /* Zero out the old sigaction struct (32 bytes on i386) */
        uint8_t *p = ctx->memory + oldact;
        if (oldact + 32 <= ctx->memory_size) {
            for (int i = 0; i < 32; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sig_rt_sigprocmask(struct yos_exec_ctx *ctx, int32_t how,
                                uint32_t set, uint32_t oset, uint32_t sigsetsize)
{
    (void)ctx; (void)how; (void)set; (void)sigsetsize;
    ydebug("rt_sigprocmask(how=%d) -> 0 (stub)\n", how);
    /* If oset is provided, zero it out */
    if (oset && ctx) {
        uint8_t *p = ctx->memory + oset;
        if (oset + 8 <= ctx->memory_size) {
            for (int i = 0; i < 8; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sigaction(struct yos_exec_ctx *ctx, int32_t signum,
                           uint32_t act, uint32_t oldact)
{
    (void)ctx;
    ydebug("sigaction(sig=%d) -> 0\n", signum);
    /* Record the wasm-side handler — see yos_sig_rt_sigaction. */
    if (act && ctx && ctx->memory && act + 4 <= ctx->memory_size) {
        uint32_t handler = *(uint32_t *)(ctx->memory + act);
        record_handler(signum, handler);
    }
    if (oldact && ctx) {
        uint8_t *p = ctx->memory + oldact;
        if (oldact + 32 <= ctx->memory_size) {
            for (int i = 0; i < 32; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sigprocmask(struct yos_exec_ctx *ctx, int32_t how,
                             uint32_t set, uint32_t oset)
{
    (void)ctx; (void)how; (void)set;
    ydebug("sigprocmask(how=%d) -> 0 (stub)\n", how);
    if (oset && ctx) {
        uint8_t *p = ctx->memory + oset;
        if (oset + 8 <= ctx->memory_size) {
            for (int i = 0; i < 8; i++) p[i] = 0;
        }
    }
    return 0;
}

/* sigsuspend — block "until a signal arrives". yos doesn't actually
 * deliver SIGCHLD on child exit (children are sibling pthreads; their
 * completion broadcasts a per-proc cond_var instead). zsh's standard
 * fork+wait pattern is:
 *
 *     block(SIGCHLD); sigsuspend(empty); waitpid(-1, ...);
 *
 * If sigsuspend doesn't block at all (the previous stub returned
 * ENOSYS), zsh interprets the unblock as a spurious wake, calls
 * sigsuspend again, and busy-loops forever — every external command
 * hangs the shell.
 *
 * Minimum-viable fix: poll the proc table for any child of ours that
 * has reached ZOMBIE state, sleeping briefly between checks. When one
 * appears (or after a short timeout — important for shells doing
 * `wait` with no children) return -1 with errno=EINTR so the caller's
 * loop falls through to waitpid. This is the same poll-with-usleep
 * shape yos_waitpid already uses (see impl/proc.c) — staying
 * consistent with existing yos process-table mechanics. */
int32_t yos_sigsuspend(struct yos_exec_ctx *ctx, uint32_t mask_off)
{
    (void)mask_off;  /* mask is irrelevant: yos doesn't deliver async signals */
    if (!ctx || !ctx->proc || !ctx->rt) {
        if (ctx && ctx->memory && ctx->errno_off)
            *(int *)(ctx->memory + ctx->errno_off) = EINVAL;
        return -1;
    }

    /* yos doesn't have async signal delivery (no kernel signals to
     * the wasm guest from outside). Instead we synthesise the most
     * important case synchronously here: if a child of the calling
     * proc has reached ZOMBIE state and the guest registered a
     * SIGCHLD handler, invoke it. zsh's wait loop predicate is
     * `jn->stat & STAT_DONE`, which gets set inside the SIGCHLD
     * handler's wait_for_processes() → waitpid(WNOHANG) reap path;
     * without this synchronous dispatch the loop spins on
     * sigsuspend/sigprocmask forever. */
    struct yos_runtime *rt = ctx->rt;
    int32_t my_pid = ctx->proc->pid;

    pthread_mutex_lock(&rt->proc_lock);
    int has_zombie = 0;
    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        struct yos_proc *p = &rt->procs[i];
        if (p->state == YOS_PROC_ZOMBIE && p->ppid == my_pid) {
            has_zombie = 1; break;
        }
    }
    pthread_mutex_unlock(&rt->proc_lock);

    if (has_zombie) {
        /* SIGCHLD = 17 on Linux but 20 on FreeBSD — zsh's wasm sees
         * the FreeBSD signal numbers (we built it against FreeBSD
         * headers). Use 20. */
        invoke_signal_handler(ctx, 20 /* SIGCHLD on FreeBSD */);
    }

    if (ctx->memory && ctx->errno_off)
        *(int *)(ctx->memory + ctx->errno_off) = EINTR;
    return -1;
}

/* ── sigemptyset / sigfillset / sigaddset / sigdelset / sigismember
 *
 * FreeBSD `sigset_t` is `__uint32_t __bits[4]` — 16 bytes total,
 * holding 128 bits (signal numbers 1..128). These are pure userspace
 * bitmap manipulators: zero/fill/set-bit/clear-bit/test-bit on the
 * 128-bit blob the wasm guest hands us. No host call, no host
 * sigset_t involved (Linux's sigset_t is 128 bytes, totally different
 * layout — we MUST do the manipulation on the wasm-side bytes
 * directly, otherwise we'd corrupt 7 sigset_ts past the boundary).
 *
 * Auto-bridge stubs these because of the size mismatch — at the
 * declaration level the bridge generator can't tell that the
 * intended manipulation is purely on the FreeBSD layout.
 */

#define YOS_FBSD_SIGSET_BYTES 16

static inline int yos_sigset_bound(struct yos_exec_ctx *ctx, uint32_t off)
{
    return off && off + YOS_FBSD_SIGSET_BYTES <= ctx->memory_size;
}

int32_t yos_sigemptyset(struct yos_exec_ctx *ctx, uint32_t set_off)
{
    if (!yos_sigset_bound(ctx, set_off)) return -EFAULT;
    memset(ctx->memory + set_off, 0, YOS_FBSD_SIGSET_BYTES);
    return 0;
}

int32_t yos_sigfillset(struct yos_exec_ctx *ctx, uint32_t set_off)
{
    if (!yos_sigset_bound(ctx, set_off)) return -EFAULT;
    memset(ctx->memory + set_off, 0xff, YOS_FBSD_SIGSET_BYTES);
    return 0;
}

/* FreeBSD sigaddset/sigdelset/sigismember:
 *   bit (signo-1) within __bits[(signo-1)/32], bit (signo-1)%32.
 * Signals are 1..128. Out-of-range returns -EINVAL. */
static int yos_sigset_op_check(int32_t signo)
{
    return (signo >= 1 && signo <= 128) ? 0 : -EINVAL;
}

int32_t yos_sigaddset(struct yos_exec_ctx *ctx, uint32_t set_off, int32_t signo)
{
    if (!yos_sigset_bound(ctx, set_off)) return -EFAULT;
    int rc = yos_sigset_op_check(signo);
    if (rc) return rc;
    uint32_t b = (uint32_t)(signo - 1);
    uint32_t *bits = (uint32_t *)(ctx->memory + set_off);
    bits[b / 32] |= 1u << (b % 32);
    return 0;
}

int32_t yos_sigdelset(struct yos_exec_ctx *ctx, uint32_t set_off, int32_t signo)
{
    if (!yos_sigset_bound(ctx, set_off)) return -EFAULT;
    int rc = yos_sigset_op_check(signo);
    if (rc) return rc;
    uint32_t b = (uint32_t)(signo - 1);
    uint32_t *bits = (uint32_t *)(ctx->memory + set_off);
    bits[b / 32] &= ~(1u << (b % 32));
    return 0;
}

int32_t yos_sigismember(struct yos_exec_ctx *ctx, uint32_t set_off, int32_t signo)
{
    if (!yos_sigset_bound(ctx, set_off)) return -EFAULT;
    int rc = yos_sigset_op_check(signo);
    if (rc) return rc;
    uint32_t b = (uint32_t)(signo - 1);
    const uint32_t *bits = (const uint32_t *)(ctx->memory + set_off);
    return (bits[b / 32] >> (b % 32)) & 1u;
}

/* ── FreeBSD ↔ host signal-number / sigset_t translation ────────────
 *
 * The guest's sigset_t is FreeBSD-shape (16 bytes, bit (signo-1) for
 * signo in 1..128). The host's sigset_t is Linux-shape (128 bytes,
 * sigemptyset/sigaddset opaque API). Many POSIX signals share the
 * same number on both kernels (HUP=1, INT=2, ...), but the BSD-vs-
 * Linux additions diverge:
 *
 *     name   FreeBSD  Linux
 *     BUS    10       7
 *     SYS    12       31
 *     URG    16       23
 *     STOP   17       19
 *     TSTP   18       20
 *     CONT   19       18
 *     CHLD   20       17
 *     IO     23       29   (Linux: SIGPOLL == SIGIO)
 *     USR1   30       10
 *     USR2   31       12
 *     EMT     7       –    (Linux has no equivalent)
 *     INFO   29       –    (Linux has no equivalent)
 *
 * The fbsd→host bit-rewrite below walks the FreeBSD bitmap and sets
 * the corresponding host bit through sigaddset(). The reverse path
 * does the same in the other direction. Signals without a host
 * equivalent are silently dropped — caller still gets EINVAL from
 * sigaddset/sigaction etc. if it tries to actually use them. */
static int fbsd_to_host_signo(int fb)
{
    /* Indexed 1..32; out-of-range returns 0 (no host equivalent). */
    static const int tbl[33] = {
        [1]  = SIGHUP,    [2]  = SIGINT,    [3]  = SIGQUIT,  [4]  = SIGILL,
        [5]  = SIGTRAP,   [6]  = SIGABRT,   [7]  = 0 /*EMT*/, [8]  = SIGFPE,
        [9]  = SIGKILL,   [10] = SIGBUS,    [11] = SIGSEGV,  [12] = SIGSYS,
        [13] = SIGPIPE,   [14] = SIGALRM,   [15] = SIGTERM,  [16] = SIGURG,
        [17] = SIGSTOP,   [18] = SIGTSTP,   [19] = SIGCONT,  [20] = SIGCHLD,
        [21] = SIGTTIN,   [22] = SIGTTOU,   [23] = SIGIO,    [24] = SIGXCPU,
        [25] = SIGXFSZ,   [26] = SIGVTALRM, [27] = SIGPROF,  [28] = SIGWINCH,
        [29] = 0 /*INFO*/,[30] = SIGUSR1,   [31] = SIGUSR2,  [32] = 0 /*THR*/,
    };
    return (fb >= 1 && fb <= 32) ? tbl[fb] : 0;
}

static int host_to_fbsd_signo(int h)
{
    for (int fb = 1; fb <= 32; fb++)
        if (fbsd_to_host_signo(fb) == h) return fb;
    return 0;
}

static void fbsd_sigset_to_host(const uint8_t *fb, sigset_t *host)
{
    sigemptyset(host);
    for (int fbsig = 1; fbsig <= 32; fbsig++) {
        int b = fbsig - 1;
        if (fb[b >> 3] & (1u << (b & 7))) {
            int h = fbsd_to_host_signo(fbsig);
            if (h > 0) sigaddset(host, h);
        }
    }
}

static void host_sigset_to_fbsd(const sigset_t *host, uint8_t *fb)
{
    memset(fb, 0, YOS_FBSD_SIGSET_BYTES);
    for (int h = 1; h < NSIG; h++) {
        if (sigismember(host, h) > 0) {
            int fbsig = host_to_fbsd_signo(h);
            if (fbsig > 0) {
                int b = fbsig - 1;
                fb[b >> 3] |= 1u << (b & 7);
            }
        }
    }
}

/* ── pthread_sigmask ─────────────────────────────────────────────────
 *
 * Layout: 16B FreeBSD sigset_t ↔ 128B host sigset_t via the bit-
 * rewrite above. `how` numbering diverges: FreeBSD SIG_BLOCK=1
 * UNBLOCK=2 SETMASK=3; Linux SIG_BLOCK=0 UNBLOCK=1 SETMASK=2.
 *
 * Return convention: POSIX pthread_sigmask returns 0 / errno, NOT
 * -1+errno. We mirror that — bridge return value IS the errno-or-
 * zero the guest's pthread_sigmask declaration expects. */
int32_t yos_pthread_sigmask(struct yos_exec_ctx *ctx, int32_t how,
                            uint32_t set_off, uint32_t oset_off)
{
    if (set_off  && !yos_sigset_bound(ctx, set_off))  return EFAULT;
    if (oset_off && !yos_sigset_bound(ctx, oset_off)) return EFAULT;

    sigset_t host_set, host_old;
    sigset_t *set_p  = NULL;
    sigset_t *oset_p = oset_off ? &host_old : NULL;
    int hhow = 0;

    if (set_off) {
        fbsd_sigset_to_host(ctx->memory + set_off, &host_set);
        set_p = &host_set;
        switch (how) {
            case 1: hhow = SIG_BLOCK;   break;
            case 2: hhow = SIG_UNBLOCK; break;
            case 3: hhow = SIG_SETMASK; break;
            default: return EINVAL;
        }
    }

    int rc = pthread_sigmask(hhow, set_p, oset_p);
    if (rc != 0) return yos_remap_errno_h2g(rc);

    if (oset_off)
        host_sigset_to_fbsd(&host_old, ctx->memory + oset_off);
    return 0;
}
