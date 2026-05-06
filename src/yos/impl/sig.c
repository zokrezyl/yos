#include "yos/types.h"
#include "yos/ydebug.h"
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>     /* usleep */

#include "wasm3.h"
#include "m3_env.h"

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
