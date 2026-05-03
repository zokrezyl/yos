/* impl/posix.c — host-libc passthrough impls for POSIX fns that
 * hooks.yaml routes to custom_<area> but for which the auto-bridge
 * isn't usable (fd-virtualisation, signature edge cases). All small,
 * mostly one-liners.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>     /* umask */
#include <sys/uio.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "yos/types.h"
#include "yos/ydebug.h"

extern int  yos_fd_alloc(struct yos_exec_ctx *ctx, int host_fd);
extern int  yos_fd_get  (struct yos_exec_ctx *ctx, int wasm_fd);
extern void yos_fd_close(struct yos_exec_ctx *ctx, int wasm_fd);

/* ── fd-remapping passthroughs ────────────────────────────────────── */

int32_t yos_dup(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    int new_hfd = dup(hfd);
    if (new_hfd < 0) return -errno;
    int new_wfd = yos_fd_alloc(ctx, new_hfd);
    if (new_wfd < 0) { close(new_hfd); return -EMFILE; }
    return new_wfd;
}

int32_t yos_isatty(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) { errno = EBADF; return 0; }
    return isatty(hfd);
}

/* fmtcheck(user_fmt, default_fmt) — FreeBSD libc helper that returns
 * `user_fmt` if its conversion specifiers are CLASS-compatible with
 * `default_fmt`, else `default_fmt`. glibc has no equivalent.
 *
 * The FreeBSD test exercises 30+ subtle cases (e.g. `%qd` is the BSD
 * synonym of `%llx`, `%D` of `%ld`, width-modifier `*` introduces an
 * extra int arg, etc.). Reimplementing it correctly from scratch is
 * a nontrivial state machine; we link against the FreeBSD source
 * directly via a thin wrapper. The .c file lives under build-tools/
 * freebsd/.../lib/libc/gen/fmtcheck.c (BSD-2-Clause). */
extern const char *yos_fmtcheck_freebsd(const char *f1, const char *f2);

uint32_t yos_fmtcheck(struct yos_exec_ctx *ctx, uint32_t user_off,
                      uint32_t default_off)
{
    const char *u = user_off    ? (const char *)(ctx->memory + user_off)    : NULL;
    const char *d = default_off ? (const char *)(ctx->memory + default_off) : NULL;
    const char *r = yos_fmtcheck_freebsd(u, d);
    /* FreeBSD always returns one of the two input pointers (or NULL
     * if user is NULL); convert that host pointer back to wasm. */
    if (r == u) return user_off;
    if (r == d) return default_off;
    return default_off;
}

int32_t yos_fsync(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    return fsync(hfd) < 0 ? -errno : 0;
}

int32_t yos_fdatasync(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    return fdatasync(hfd) < 0 ? -errno : 0;
}

int32_t yos_fchdir(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    return fchdir(hfd) < 0 ? -errno : 0;
}

/* ── pread / pwrite (host has same signature, just need fd remap) ── */

int32_t yos_pread(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t buf,
                  uint32_t count, int64_t off)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    if (buf + count > ctx->memory_size) return -EFAULT;
    ssize_t r = pread(hfd, ctx->memory + buf, count, (off_t)off);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_pwrite(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t buf,
                   uint32_t count, int64_t off)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    if (buf + count > ctx->memory_size) return -EFAULT;
    ssize_t r = pwrite(hfd, ctx->memory + buf, count, (off_t)off);
    return r < 0 ? -errno : (int32_t)r;
}

/* ── trivial host passthroughs (no fd, no memory) ─────────────────── */

int32_t yos_getuid (struct yos_exec_ctx *ctx) { (void)ctx; return (int32_t)getuid();  }
int32_t yos_geteuid(struct yos_exec_ctx *ctx) { (void)ctx; return (int32_t)geteuid(); }
int32_t yos_getgid (struct yos_exec_ctx *ctx) { (void)ctx; return (int32_t)getgid();  }
int32_t yos_getegid(struct yos_exec_ctx *ctx) { (void)ctx; return (int32_t)getegid(); }

uint32_t yos_umask(struct yos_exec_ctx *ctx, uint32_t mask)
{
    (void)ctx;
    return (uint32_t)umask((mode_t)mask);
}

int32_t yos_sync(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    sync();
    return 0;
}

/* ── signals ──────────────────────────────────────────────────────── */

int32_t yos_raise(struct yos_exec_ctx *ctx, int32_t sig)
{
    (void)ctx;
    return raise(sig) < 0 ? -errno : 0;
}

int32_t yos_killpg(struct yos_exec_ctx *ctx, int32_t pgrp, int32_t sig)
{
    (void)ctx;
    return killpg(pgrp, sig) < 0 ? -errno : 0;
}

/* ── sbrk: thin proxy to brk-style heap. yos_brk lives in impl/mem.c
 * and tracks ctx->heap_end. sbrk(0) reports current break, sbrk(N)
 * advances and returns the OLD break. */

extern int32_t yos_brk(struct yos_exec_ctx *ctx, uint32_t addr);

uint32_t yos_sbrk(struct yos_exec_ctx *ctx, int32_t incr)
{
    uint32_t old = ctx->heap_end;
    if (incr == 0) return old;
    int32_t r = yos_brk(ctx, old + (uint32_t)incr);
    if (r < 0) return (uint32_t)-1;
    return old;
}

/* ── socketpair — host has identical signature; bridge can't render
 * the int sv[2] arg auto-style, do it here. */

int32_t yos_socketpair(struct yos_exec_ctx *ctx, int32_t domain,
                       int32_t type, int32_t protocol, uint32_t sv_off)
{
    if (sv_off + 8 > ctx->memory_size) return -EFAULT;
    int hfds[2];
    if (socketpair(domain, type, protocol, hfds) < 0) return -errno;
    int32_t wa = yos_fd_alloc(ctx, hfds[0]);
    if (wa < 0) { close(hfds[0]); close(hfds[1]); return -EMFILE; }
    int32_t wb = yos_fd_alloc(ctx, hfds[1]);
    if (wb < 0) { yos_fd_close(ctx, wa); close(hfds[1]); return -EMFILE; }
    int32_t *sv = (int32_t *)(ctx->memory + sv_off);
    sv[0] = wa;
    sv[1] = wb;
    return 0;
}
