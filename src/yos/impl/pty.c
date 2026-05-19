/* impl/pty.c — pseudo-terminal bridges.
 *
 * The codegen leaves posix_openpt / grantpt / unlockpt / ptsname /
 * ptsname_r as ENOSYS stubs because the header walk's wasm32 target
 * disables the pty fns (they're conditioned on _XOPEN_SOURCE plus
 * various platform feature macros that don't fire on our extractor
 * invocation). telnetd's PTY allocation path hits these straight
 * away — without bridges it prints
 *
 *     telnetd: All network ports in use
 *
 * and exits before any IAC negotiation reaches the client.
 *
 * Hand-bridge here. The host platforms we care about (Linux/glibc,
 * darwin/libSystem, FreeBSD) all expose these via <stdlib.h>; just
 * pass through. The wasm fd returned by posix_openpt is allocated
 * through yos's fd table so subsequent read/write/ioctl bridges find
 * it; ptsname returns the host string verbatim copied into a TLS
 * scratch buffer, identical lifetime contract to host ptsname.
 */

#define _XOPEN_SOURCE 600    /* posix_openpt + friends */
#define _GNU_SOURCE
#if defined(__APPLE__)
/* darwin's <stdlib.h> gates ptsname_r behind
 * (!_POSIX_C_SOURCE || _DARWIN_C_SOURCE); _XOPEN_SOURCE above
 * sets _POSIX_C_SOURCE so we have to re-open the door. */
#  define _DARWIN_C_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include "yos/types.h"
#include <yos/ytrace/ytrace.h>
#include "impl/errno_helpers.h"

extern int yos_fd_alloc(struct yos_exec_ctx *, int);
extern int yos_fd_get  (struct yos_exec_ctx *, int);

/* ── posix_openpt(int flags) ─ FreeBSD-shape flags. Mostly O_RDWR
 * (= 0x0002 on FreeBSD-i386, also 0x0002 on Linux/darwin) plus
 * sometimes O_NOCTTY (= 0x8000 FreeBSD, 0x100 Linux). For telnetd
 * the only flag passed is O_RDWR — we translate via the same
 * oflags_fb_to_lx helper used by yos_open, but to keep this file
 * standalone (the helper is static to vfs.c) we just map the two
 * combos we expect inline. */
int32_t yos_posix_openpt(struct yos_exec_ctx *ctx, int32_t fb_flags)
{
    int host_flags = O_RDWR;
    /* FreeBSD-i386 O_NOCTTY = 0x8000; we don't need it for telnetd
     * but pass it through if seen. */
    if (fb_flags & 0x8000) host_flags |= O_NOCTTY;
    int hfd = posix_openpt(host_flags);
    if (hfd < 0) return yos_errno_neg(ctx, errno);
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) { close(hfd); return yos_errno_neg(ctx, ENFILE); }
    ydebug("posix_openpt(0x%x→0x%x) = wfd=%d hfd=%d\n",
           fb_flags, host_flags, wfd, hfd);
    return wfd;
}

int32_t yos_grantpt(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_neg(ctx, EBADF);
    int rc = grantpt(hfd);
    ydebug("grantpt(wfd=%d hfd=%d) = %d errno=%d\n", wfd, hfd, rc, rc < 0 ? errno : 0);
    if (rc < 0) return yos_errno_neg(ctx, errno);
    return 0;
}

int32_t yos_unlockpt(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_neg(ctx, EBADF);
    int rc = unlockpt(hfd);
    ydebug("unlockpt(wfd=%d hfd=%d) = %d errno=%d\n", wfd, hfd, rc, rc < 0 ? errno : 0);
    if (rc < 0) return yos_errno_neg(ctx, errno);
    return 0;
}

/* ptsname returns a pointer to a static buffer in libc. We mirror by
 * copying into a per-thread wasm scratch slab whose lifetime matches
 * the next ptsname call from the same guest thread. Caller's wasm-
 * side `char *` view sees the same string contents. */
#define YOS_PTSNAME_BUF 256
uint32_t yos_ptsname(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_null(ctx, EBADF);
    char *hs = ptsname(hfd);
    if (!hs) return yos_errno_null(ctx, errno);
    /* Allocate a wasm-side scratch via yos_malloc once, reuse on
     * subsequent calls. (Thread-local because the wasm caller's
     * libc treats the returned pointer as having the same lifetime
     * as host ptsname's static buf.) */
    static _Thread_local uint32_t buf_off = 0;
    extern uint32_t yos_malloc(struct yos_exec_ctx *, uint32_t);
    if (!buf_off) buf_off = yos_malloc(ctx, YOS_PTSNAME_BUF);
    if (!buf_off) return yos_errno_null(ctx, ENOMEM);
    size_t n = strlen(hs);
    if (n >= YOS_PTSNAME_BUF) n = YOS_PTSNAME_BUF - 1;
    memcpy(ctx->memory + buf_off, hs, n);
    ctx->memory[buf_off + n] = 0;
    ydebug("ptsname(wfd=%d hfd=%d) = wasm_off=0x%x \"%s\"\n",
           wfd, hfd, buf_off, hs);
    return buf_off;
}

int32_t yos_ptsname_r(struct yos_exec_ctx *ctx, int32_t wfd,
                      uint32_t buf_off, uint32_t buflen)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return EBADF;
    if (!buf_off || buf_off + buflen > ctx->memory_size) return EFAULT;
    char *guest = (char *)(ctx->memory + buf_off);
    int rc = ptsname_r(hfd, guest, (size_t)buflen);
    if (rc != 0) return rc;
    return 0;
}
