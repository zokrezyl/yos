/* impl/posix.c — host-libc passthrough impls for POSIX fns that
 * hooks.yaml routes to custom_<area> but for which the auto-bridge
 * isn't usable (fd-virtualisation, signature edge cases). All small,
 * mostly one-liners.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>      /* posix_madvise */

#if defined(__APPLE__)
/* darwin has no fdatasync; fsync is the closest equivalent (it does
 * the same data-and-metadata flush, and F_FULLFSYNC is a stronger
 * variant). Map fdatasync -> fsync for the host. */
#  define fdatasync(fd) fsync(fd)
/* SOCK_CLOEXEC / SOCK_NONBLOCK are Linux extensions to socket(2); darwin
 * has no equivalent flags-on-socket-create. Define as 0 so the bit-or
 * compiles; the atomic semantics are lost — call sites that need them
 * must follow up with fcntl(F_SETFD, FD_CLOEXEC) / fcntl(F_SETFL, O_NONBLOCK).
 * TODO: do that fcntl postwork in yos_socketpair on darwin. */
#  ifndef SOCK_CLOEXEC
#    define SOCK_CLOEXEC 0
#  endif
#  ifndef SOCK_NONBLOCK
#    define SOCK_NONBLOCK 0
#  endif
#endif
#include <sys/socket.h>
#include <sys/stat.h>     /* umask */
#include <sys/uio.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#include "yos/types.h"
#include "yos/ydebug.h"
#include "platform.h"

extern int  yos_fd_alloc(struct yos_exec_ctx *ctx, int host_fd);
extern int  yos_fd_get  (struct yos_exec_ctx *ctx, int wasm_fd);
extern void yos_fd_close(struct yos_exec_ctx *ctx, int wasm_fd);

/* ── fd-remapping passthroughs ────────────────────────────────────── */

int32_t yos_dup(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    ydebug("dup(wfd=%d hfd=%d)\n", wfd, hfd);
    if (hfd < 0) return -EBADF;
    int new_hfd = dup(hfd);
    if (new_hfd < 0) {
        ydebug("dup(wfd=%d hfd=%d) host dup failed: %s\n",
               wfd, hfd, strerror(errno));
        return -errno;
    }
    int new_wfd = yos_fd_alloc(ctx, new_hfd);
    if (new_wfd < 0) { close(new_hfd); return -EMFILE; }
    ydebug("dup(wfd=%d hfd=%d) -> new_wfd=%d new_hfd=%d\n",
           wfd, hfd, new_wfd, new_hfd);
    return new_wfd;
}

int32_t yos_isatty(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) { errno = EBADF; ydebug("isatty(wfd=%d) -> 0 (EBADF)\n", wfd); return 0; }
    int r = isatty(hfd);
    ydebug("isatty(wfd=%d hfd=%d) -> %d\n", wfd, hfd, r);
    return r;
}

/* yos_getsockname — translate host fd, then convert host's Linux
 * sockaddr layout (sa_family uint16 @0) to wasm's FreeBSD layout
 * (sa_len uint8 @0, sa_family uint8 @1). socklen_t is 4 bytes both
 * sides — the wasm slot is fine. nvim/libuv's uv_guess_handle reads
 * ss_family at FreeBSD offset 1 to detect AF_UNIX spawn pipes; without
 * the conversion it sees 0 (the high byte of Linux's family field),
 * decides the fd is UV_UNKNOWN_HANDLE and asserts in stream_init. */
int32_t yos_getsockname(struct yos_exec_ctx *ctx, int32_t wfd,
                        uint32_t addr_off, uint32_t addrlen_off)
{
    int hfd = yos_fd_get(ctx, wfd);
    ydebug("getsockname(wfd=%d hfd=%d)\n", wfd, hfd);
    if (hfd < 0) return -EBADF;
    uint32_t *addrlen_p = (uint32_t *)(ctx->memory + addrlen_off);
    socklen_t cap = (socklen_t)*addrlen_p;
    if (cap > 256) cap = 256;  /* cap; libuv only needs the family */
    uint8_t host_buf[256];
    socklen_t host_len = cap;
    if (getsockname(hfd, (struct sockaddr *)host_buf, &host_len) < 0) {
        ydebug("getsockname host failed: %s\n", strerror(errno));
        return -errno;
    }
    /* Decode the host family. Linux: sa_family is uint16_t at offset
     * 0. BSD-lineage hosts (darwin/FreeBSD) put sa_len uint8 at 0,
     * sa_family uint8 at 1. The wrong decoding gave nvim's
     * uv_guess_handle ss_family = sa_len (e.g. 16) on darwin instead
     * of AF_UNIX, classifying the IPC socketpair end as
     * UV_UNKNOWN_HANDLE — root cause of "ch 1 was closed by the
     * client" in tmp/nvim-runtime-issues.md. */
#if defined(__APPLE__) || defined(__FreeBSD__)
    uint16_t host_fam = (uint16_t)host_buf[1];
#else
    uint16_t host_fam = (uint16_t)(host_buf[0] | (host_buf[1] << 8));
#endif
    /* Re-emit FreeBSD shape: sa_len, sa_family[, sa_data...]. */
    uint8_t *w = ctx->memory + addr_off;
    socklen_t out = host_len < cap ? host_len : cap;
    if (out >= 2) {
        w[0] = (uint8_t)out;          /* sa_len */
        w[1] = (uint8_t)(host_fam & 0xff); /* sa_family */
        if (out > 2)
            memcpy(w + 2, host_buf + 2, out - 2);
    }
    *addrlen_p = (uint32_t)host_len;
    ydebug("getsockname -> family=%u out_len=%u\n",
           (unsigned)host_fam, (unsigned)out);
    return 0;
}

/* SOL_SOCKET + SO_TYPE constants differ between FreeBSD and Linux.
 *   FreeBSD: SOL_SOCKET=0xffff, SO_TYPE=0x1008, SO_ERROR=0x1007,
 *            SO_REUSEADDR=0x4, SO_KEEPALIVE=0x8, SO_BROADCAST=0x20,
 *            SO_LINGER=0x80, SO_SNDBUF=0x1001, SO_RCVBUF=0x1002,
 *            SO_SNDLOWAT=0x1003, SO_RCVLOWAT=0x1004, SO_SNDTIMEO=0x1005,
 *            SO_RCVTIMEO=0x1006, SO_OOBINLINE=0x100, SO_ACCEPTCONN=0x2.
 *   Linux:   SOL_SOCKET=1,      SO_TYPE=3,      SO_ERROR=4,
 *            SO_REUSEADDR=2, SO_KEEPALIVE=9, SO_BROADCAST=6,
 *            SO_LINGER=13, SO_SNDBUF=7, SO_RCVBUF=8,
 *            SO_SNDLOWAT=19, SO_RCVLOWAT=18, SO_SNDTIMEO=21,
 *            SO_RCVTIMEO=20, SO_OOBINLINE=10, SO_ACCEPTCONN=30.
 * Only the few that nvim/libuv actually exercise are translated below;
 * unknown options pass through and may EINVAL. */
static int sol_fb_to_lx(int level)
{
    if (level == 0xffff) return SOL_SOCKET;
    return level;
}
static int soopt_fb_to_lx(int level, int opt)
{
    if (level != 0xffff) return opt;
    switch (opt) {
    case 0x1008: return SO_TYPE;
    case 0x1007: return SO_ERROR;
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0020: return SO_BROADCAST;
    case 0x0080: return SO_LINGER;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO;
    case 0x0100: return SO_OOBINLINE;
    case 0x0002: return SO_ACCEPTCONN;
    default:     return opt;
    }
}

int32_t yos_getsockopt(struct yos_exec_ctx *ctx, int32_t wfd, int32_t level,
                       int32_t opt, uint32_t valbuf, uint32_t lenptr)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    int hlevel = sol_fb_to_lx(level);
    int hopt = soopt_fb_to_lx(level, opt);
    void *vbuf = ctx->memory + valbuf;
    socklen_t *lp = (socklen_t *)(ctx->memory + lenptr);
    int r = getsockopt(hfd, hlevel, hopt, vbuf, lp);
    return r < 0 ? -errno : r;
}

int32_t yos_setsockopt(struct yos_exec_ctx *ctx, int32_t wfd, int32_t level,
                       int32_t opt, uint32_t valbuf, uint32_t valbuflen)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    int hlevel = sol_fb_to_lx(level);
    int hopt = soopt_fb_to_lx(level, opt);
    const void *vbuf = ctx->memory + valbuf;
    int r = setsockopt(hfd, hlevel, hopt, vbuf, valbuflen);
    return r < 0 ? -errno : r;
}

extern int sock_type_fb_to_lx_fwd(int t);  /* defined below */

int32_t yos_socket(struct yos_exec_ctx *ctx, int32_t domain, int32_t type, int32_t protocol)
{
    int htype = sock_type_fb_to_lx_fwd(type);
    int hfd = socket(domain, htype, protocol);
    if (hfd < 0) return -errno;
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) { close(hfd); return -EMFILE; }
    return wfd;
}

/* Socket-side bridges that need wfd → hfd translation. The auto-
 * bridge for these passes the wasm fd straight through to the host
 * which sees an unrelated fd (or EBADF) — that's what makes the
 * coverage probe's `bind(127.0.0.1:0)` and `listen()` fail with
 * errno=9 (EBADF). */
/* FreeBSD sockaddr_in (and sockaddr_in6) put a 1-byte sin_len at
 * offset 0 and sa_family at offset 1. Linux drops sin_len and uses
 * a 2-byte sin_family at offset 0. Without translation, the host
 * bind/connect/sendto see family=0x0002 (sin_len=2 in the low
 * byte, AF_INET=2 in the high byte → 0x0202, an unknown family)
 * and silently fail. Same for sockaddr_in6 (family always at
 * offset 0/1 regardless, but sin6_len needs stripping). */
static void freebsd_sockaddr_to_host(uint8_t *buf, socklen_t len)
{
#ifndef __FreeBSD__
#ifndef __APPLE__
    if (len >= 2) {
        uint8_t sin_len_byte    = buf[0];
        uint8_t sin_family_byte = buf[1];
        (void)sin_len_byte;
        /* Linux: sin_family is uint16 little-endian at offset 0/1.
         * Just zero the high byte and put sa_family in the low. */
        buf[0] = sin_family_byte;
        buf[1] = 0;
    }
#endif
#endif
}

int32_t yos_bind(struct yos_exec_ctx *ctx, int32_t fd, uint32_t addr_off,
                 uint32_t addrlen)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    if (addr_off >= ctx->memory_size) return -EFAULT;
    /* Make a host-shape copy of the address and convert in-place. */
    uint8_t hostbuf[256];
    if (addrlen > sizeof(hostbuf)) return -EINVAL;
    memcpy(hostbuf, ctx->memory + addr_off, addrlen);
    freebsd_sockaddr_to_host(hostbuf, (socklen_t)addrlen);
    return bind(hfd, (struct sockaddr *)hostbuf, (socklen_t)addrlen) < 0
           ? -errno : 0;
}

int32_t yos_listen(struct yos_exec_ctx *ctx, int32_t fd, int32_t backlog)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    return listen(hfd, backlog) < 0 ? -errno : 0;
}

int32_t yos_connect(struct yos_exec_ctx *ctx, int32_t fd, uint32_t addr_off,
                    uint32_t addrlen)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    if (addr_off >= ctx->memory_size) return -EFAULT;
    uint8_t hostbuf[256];
    if (addrlen > sizeof(hostbuf)) return -EINVAL;
    memcpy(hostbuf, ctx->memory + addr_off, addrlen);
    freebsd_sockaddr_to_host(hostbuf, (socklen_t)addrlen);
    return connect(hfd, (struct sockaddr *)hostbuf, (socklen_t)addrlen) < 0
           ? -errno : 0;
}

int32_t yos_accept(struct yos_exec_ctx *ctx, int32_t fd, uint32_t addr_off,
                   uint32_t addrlen_off)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    socklen_t hlen = 0;
    socklen_t *hlen_p = NULL;
    struct sockaddr *haddr = NULL;
    if (addr_off && addrlen_off &&
        addr_off < ctx->memory_size && addrlen_off + 4 <= ctx->memory_size) {
        hlen   = (socklen_t)*(uint32_t *)(ctx->memory + addrlen_off);
        hlen_p = &hlen;
        haddr  = (struct sockaddr *)(ctx->memory + addr_off);
    }
    int newhfd = accept(hfd, haddr, hlen_p);
    if (newhfd < 0) return -errno;
    if (addrlen_off) *(uint32_t *)(ctx->memory + addrlen_off) = (uint32_t)hlen;
    int newwfd = yos_fd_alloc(ctx, newhfd);
    if (newwfd < 0) { close(newhfd); return -EMFILE; }
    return newwfd;
}

ssize_t yos_send(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf_off,
                 uint32_t len, int32_t flags)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    if (buf_off >= ctx->memory_size) return -EFAULT;
    ssize_t n = send(hfd, ctx->memory + buf_off, len, flags);
    return n < 0 ? -errno : n;
}

ssize_t yos_recv(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf_off,
                 uint32_t len, int32_t flags)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    if (buf_off >= ctx->memory_size) return -EFAULT;
    ssize_t n = recv(hfd, ctx->memory + buf_off, len, flags);
    return n < 0 ? -errno : n;
}

ssize_t yos_sendto(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf_off,
                   uint32_t len, int32_t flags, uint32_t dst_off,
                   uint32_t dst_len)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    const struct sockaddr *dst = NULL;
    if (dst_off && dst_off < ctx->memory_size)
        dst = (const struct sockaddr *)(ctx->memory + dst_off);
    ssize_t n = sendto(hfd, ctx->memory + buf_off, len, flags, dst,
                       (socklen_t)dst_len);
    return n < 0 ? -errno : n;
}

ssize_t yos_recvfrom(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf_off,
                     uint32_t len, int32_t flags, uint32_t src_off,
                     uint32_t srclen_off)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    socklen_t hlen = 0;
    socklen_t *hlen_p = NULL;
    struct sockaddr *src = NULL;
    if (src_off && srclen_off && srclen_off + 4 <= ctx->memory_size) {
        hlen   = (socklen_t)*(uint32_t *)(ctx->memory + srclen_off);
        hlen_p = &hlen;
        src    = (struct sockaddr *)(ctx->memory + src_off);
    }
    ssize_t n = recvfrom(hfd, ctx->memory + buf_off, len, flags, src, hlen_p);
    if (n < 0) return -errno;
    if (srclen_off) *(uint32_t *)(ctx->memory + srclen_off) = (uint32_t)hlen;
    return n;
}

int32_t yos_shutdown(struct yos_exec_ctx *ctx, int32_t fd, int32_t how)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    return shutdown(hfd, how) < 0 ? -errno : 0;
}

int32_t yos_getpeername(struct yos_exec_ctx *ctx, int32_t fd,
                        uint32_t addr_off, uint32_t addrlen_off)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    if (!addr_off || !addrlen_off) return -EFAULT;
    socklen_t hlen = (socklen_t)*(uint32_t *)(ctx->memory + addrlen_off);
    if (getpeername(hfd, (struct sockaddr *)(ctx->memory + addr_off), &hlen) < 0)
        return -errno;
    *(uint32_t *)(ctx->memory + addrlen_off) = (uint32_t)hlen;
    return 0;
}

/* posix_fadvise / posix_fallocate — fd-taking; the auto-bridge
 * passed the wasm fd straight to host which then sees EBADF. They
 * return the error code as the function value (not via errno). */
int32_t yos_posix_fadvise(struct yos_exec_ctx *ctx, int32_t fd,
                          int64_t offset, int64_t len, int32_t advice)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return -hfd;  /* return positive error code */
    return yos_plat_posix_fadvise(hfd, (off_t)offset, (off_t)len, advice);
}

int32_t yos_posix_fallocate(struct yos_exec_ctx *ctx, int32_t fd,
                            int64_t offset, int64_t len)
{
    int hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return -hfd;
    return yos_plat_posix_fallocate(hfd, (off_t)offset, (off_t)len);
}

/* lpathconf — FreeBSD-specific; Linux only has pathconf (which
 * follows symlinks; lpathconf doesn't). Map to host lstat-then-
 * pathconf for the typical _PC_ACL_* / _PC_LINK_MAX queries. ls
 * calls lpathconf(name, _PC_ACL_NFS4) for every entry; without a
 * bridge it returned -1 with ENOSYS in wasm errno → ls's check
 * `errno != EINVAL` was true → ls warned `<name>: Invalid argument`
 * for every entry, then exited. Returning EINVAL (which is the
 * error Linux pathconf produces for ACL queries on filesystems
 * without ACL support) makes ls skip the warn cleanly. */
int32_t yos_lpathconf(struct yos_exec_ctx *ctx, uint32_t path_off, int32_t name)
{
    if (!path_off) { errno = EFAULT; return -1; }
    const char *path = (const char *)(ctx->memory + path_off);
    /* Try host pathconf — works for many _PC_* values (LINK_MAX,
     * NAME_MAX, …). Linux has no lpathconf; pathconf follows
     * symlinks but for ls's use case that's fine. */
    long r = pathconf(path, name);
    if (r < 0) {
        /* Many FreeBSD-only _PC_* (NFS4 ACL, ACL_PATH_MAX) return -1
         * on Linux with EINVAL, which is exactly what ls expects to
         * mean "no ACLs". Pass that through. */
        return -1;
    }
    return (int32_t)r;
}

#include <netdb.h>

/* getaddrinfo — auto-bridge stubs this with -ENOSYS. ssh, scp, sftp,
 * curl, anything that resolves a hostname needs it. We translate the
 * wasm-side hints struct, call host getaddrinfo, then build a wasm-
 * side `struct addrinfo` linked-list inside the wasm linear memory
 * via yos_malloc. ssh's freeaddrinfo is bridged separately to walk
 * and free that wasm-side list.
 *
 * FreeBSD wasm32 struct addrinfo (sizeof = 32):
 *   off  0: int  ai_flags
 *   off  4: int  ai_family
 *   off  8: int  ai_socktype
 *   off 12: int  ai_protocol
 *   off 16: socklen_t ai_addrlen   (4)
 *   off 20: char *ai_canonname     (4)
 *   off 24: struct sockaddr *ai_addr (4)
 *   off 28: struct addrinfo *ai_next (4)
 *
 * sockaddr_in is 16 bytes (FreeBSD: sin_len 1, sin_family 1, sin_port 2,
 * sin_addr 4, sin_zero[8]). The bridge writes sockaddr_in / in6 to the
 * wasm side in FreeBSD layout (sin_len byte then family byte).
 */

extern uint32_t yos_malloc(struct yos_exec_ctx *ctx, uint32_t size);

#define WASM_ADDRINFO_SZ 32u

static void host_sockaddr_to_freebsd(uint8_t *out, const struct sockaddr *src,
                                     socklen_t len)
{
    memcpy(out, src, len);
    /* Convert host sa_family (uint16 @0) to FreeBSD sa_len@0,
     * sa_family@1. */
    if (len >= 2) {
        uint16_t fam = ((uint16_t)((unsigned char)out[0])) |
                       (((uint16_t)((unsigned char)out[1])) << 8);
        out[0] = (uint8_t)len;
        out[1] = (uint8_t)(fam & 0xff);
    }
}

int32_t yos_getaddrinfo(struct yos_exec_ctx *ctx, uint32_t node_off,
                        uint32_t service_off, uint32_t hints_off,
                        uint32_t res_off)
{
    if (!res_off || res_off + 4 > ctx->memory_size) return EAI_SYSTEM;
    const char *node    = node_off    ? (const char *)(ctx->memory + node_off)    : NULL;
    const char *service = service_off ? (const char *)(ctx->memory + service_off) : NULL;

    struct addrinfo host_hints = {0};
    struct addrinfo *host_hints_p = NULL;
    if (hints_off && hints_off + WASM_ADDRINFO_SZ <= ctx->memory_size) {
        const uint8_t *w = ctx->memory + hints_off;
        host_hints.ai_flags    = *(int32_t *)(w +  0);
        host_hints.ai_family   = *(int32_t *)(w +  4);
        host_hints.ai_socktype = *(int32_t *)(w +  8);
        host_hints.ai_protocol = *(int32_t *)(w + 12);
        host_hints_p = &host_hints;
    }

    struct addrinfo *host_res = NULL;
    int rc = getaddrinfo(node, service, host_hints_p, &host_res);
    if (rc != 0) {
        *(uint32_t *)(ctx->memory + res_off) = 0;
        return rc;
    }

    /* Build wasm-side linked list. Each entry: struct + sockaddr +
     * canonname (if present) — all in one yos_malloc per node so the
     * matching freeaddrinfo can release the lot. */
    uint32_t first = 0;
    uint32_t prev = 0;
    for (struct addrinfo *p = host_res; p; p = p->ai_next) {
        uint32_t addrlen = p->ai_addrlen;
        size_t namelen = p->ai_canonname ? strlen(p->ai_canonname) + 1 : 0;
        uint32_t total = WASM_ADDRINFO_SZ + addrlen + (uint32_t)namelen;
        uint32_t blk = yos_malloc(ctx, total);
        if (!blk) { freeaddrinfo(host_res); return EAI_MEMORY; }
        uint8_t *w = ctx->memory + blk;
        memset(w, 0, total);
        *(int32_t *)(w +  0) = p->ai_flags;
        *(int32_t *)(w +  4) = p->ai_family;
        *(int32_t *)(w +  8) = p->ai_socktype;
        *(int32_t *)(w + 12) = p->ai_protocol;
        *(uint32_t *)(w + 16) = addrlen;
        uint32_t addr_off = blk + WASM_ADDRINFO_SZ;
        host_sockaddr_to_freebsd(ctx->memory + addr_off, p->ai_addr, addrlen);
        *(uint32_t *)(w + 24) = addr_off;
        if (namelen) {
            uint32_t name_off = addr_off + addrlen;
            memcpy(ctx->memory + name_off, p->ai_canonname, namelen);
            *(uint32_t *)(w + 20) = name_off;
        }
        /* ai_next */
        if (prev) *(uint32_t *)(ctx->memory + prev + 28) = blk;
        else      first = blk;
        prev = blk;
    }
    freeaddrinfo(host_res);
    *(uint32_t *)(ctx->memory + res_off) = first;
    return 0;
}

/* freeaddrinfo — walk the wasm-side list, yos_free each block. */
extern void yos_free(struct yos_exec_ctx *ctx, uint32_t off);

void yos_freeaddrinfo(struct yos_exec_ctx *ctx, uint32_t res_off)
{
    while (res_off && res_off + WASM_ADDRINFO_SZ <= ctx->memory_size) {
        uint32_t next = *(uint32_t *)(ctx->memory + res_off + 28);
        yos_free(ctx, res_off);
        res_off = next;
    }
}

/* getnameinfo — host call with FreeBSD→host sockaddr conversion. */
int32_t yos_getnameinfo(struct yos_exec_ctx *ctx, uint32_t sa_off,
                        uint32_t salen, uint32_t host_off, uint32_t hostlen,
                        uint32_t serv_off, uint32_t servlen, int32_t flags)
{
    if (!sa_off || sa_off + salen > ctx->memory_size) return EAI_SYSTEM;
    uint8_t hostbuf_sa[256];
    if (salen > sizeof(hostbuf_sa)) return EAI_SYSTEM;
    memcpy(hostbuf_sa, ctx->memory + sa_off, salen);
    /* FreeBSD layout → Linux: sa_family at offset 0/1 (low byte). */
    if (salen >= 2) { uint8_t fam = hostbuf_sa[1]; hostbuf_sa[0] = fam; hostbuf_sa[1] = 0; }
    char *hbuf = host_off ? (char *)(ctx->memory + host_off) : NULL;
    char *sbuf = serv_off ? (char *)(ctx->memory + serv_off) : NULL;
    return getnameinfo((const struct sockaddr *)hostbuf_sa, (socklen_t)salen,
                       hbuf, (socklen_t)hostlen, sbuf, (socklen_t)servlen, flags);
}

/* posix_madvise — auto-bridge passes wasm offset converted to host
 * pointer, but the conversion happens on EVERY call without checking
 * the offset's validity, and certain Linux versions reject our addr
 * range with EINVAL because the bridge passes addr without rounding
 * to a page boundary. Hand-bridge to round addr/len to the page
 * boundary on the host side. */
int32_t yos_posix_madvise(struct yos_exec_ctx *ctx, uint32_t addr_off,
                          uint32_t len, int32_t advice)
{
    if (!addr_off || addr_off + len > ctx->memory_size) return EINVAL;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t host_addr = (uintptr_t)(ctx->memory + addr_off);
    uintptr_t aligned = host_addr & ~((uintptr_t)ps - 1);
    size_t pad = host_addr - aligned;
    /* posix_madvise returns the errno-style code directly. */
    return posix_madvise((void *)aligned, len + pad, advice);
}

/* getloadavg — auto-bridge stubs this with -ENOSYS because the
 * output type is `double *`. Hand-bridge: take a wasm offset to an
 * array of nelem doubles, write the values directly into wasm
 * memory. */
int32_t yos_getloadavg(struct yos_exec_ctx *ctx, uint32_t loadavg_off,
                       int32_t nelem)
{
    if (nelem <= 0) return 0;
    if (nelem > 3) nelem = 3;
    if (!loadavg_off ||
        loadavg_off + (uint32_t)nelem * 8 > ctx->memory_size)
        return -EFAULT;
    double host[3];
    int n = getloadavg(host, nelem);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        memcpy(ctx->memory + loadavg_off + (uint32_t)i * 8, &host[i], 8);
    return n;
}

#include <stdlib.h>  /* mkdtemp/mkstemp */

/* realpath: resolve absolute pathname. Two calling conventions:
 *   resolved_off != 0: write into the guest-supplied buffer (FreeBSD
 *                       PATH_MAX = 1024). Return resolved_off on
 *                       success, 0 on error.
 *   resolved_off == 0: caller wants malloc'd buffer. We don't have a
 *                       guest-side allocator hooked up here, so today
 *                       we return 0 + errno=ENOMEM. The FreeBSD-base
 *                       realpath(1) tool always passes a non-NULL buf,
 *                       which is the case we care about.
 * The auto-bridge stubs this to NULL because the result is "alias of
 * second arg" — a pattern the generator doesn't yet recognise. */
uint32_t yos_realpath(struct yos_exec_ctx *ctx, uint32_t path_off,
                      uint32_t resolved_off)
{
    if (!path_off || path_off >= ctx->memory_size) {
        if (ctx && ctx->memory && ctx->errno_off)
            *(int *)(ctx->memory + ctx->errno_off) = EFAULT;
        return 0;
    }
    const char *p = (const char *)(ctx->memory + path_off);
    if (!resolved_off) {
        /* malloc-style call — not yet supported here. */
        if (ctx && ctx->memory && ctx->errno_off)
            *(int *)(ctx->memory + ctx->errno_off) = ENOMEM;
        return 0;
    }
    if (resolved_off >= ctx->memory_size) {
        if (ctx && ctx->memory && ctx->errno_off)
            *(int *)(ctx->memory + ctx->errno_off) = EFAULT;
        return 0;
    }
    char *buf = (char *)(ctx->memory + resolved_off);
    char *r = realpath(p, buf);
    if (!r) {
        extern int yos_remap_errno_h2g(int);
        if (ctx && ctx->memory && ctx->errno_off)
            *(int *)(ctx->memory + ctx->errno_off) = yos_remap_errno_h2g(errno);
        return 0;
    }
    return resolved_off;
}

/* mkdtemp: replaces the trailing "XXXXXX" in template with random
 * chars, creates the directory, returns the (modified) template
 * pointer on success or NULL on failure. The auto-bridge stubbed
 * this to NULL — fatal for nvim's swap-file machinery. */
uint32_t yos_mkdtemp(struct yos_exec_ctx *ctx, uint32_t template_off)
{
    if (!template_off || template_off >= ctx->memory_size) return 0;
    char *t = (char *)(ctx->memory + template_off);
    /* Bound the in-place modification to the readable region.
     * mkdtemp writes back into the same buffer, replacing X chars. */
    char *r = mkdtemp(t);
    if (!r) return 0;
    return template_off;
}

int32_t yos_mkstemp(struct yos_exec_ctx *ctx, uint32_t template_off)
{
    if (!template_off || template_off >= ctx->memory_size) return -EFAULT;
    char *t = (char *)(ctx->memory + template_off);
    int hfd = mkstemp(t);
    if (hfd < 0) return -errno;
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) { close(hfd); return -EMFILE; }
    return wfd;
}

int32_t yos_mkostemp(struct yos_exec_ctx *ctx, uint32_t template_off, int32_t flags)
{
    if (!template_off || template_off >= ctx->memory_size) return -EFAULT;
    char *t = (char *)(ctx->memory + template_off);
    /* flags here are FreeBSD O_* (e.g. O_CLOEXEC) — translate to host. */
    extern int oflags_fb_to_lx_fwd(int);
    int hflags = oflags_fb_to_lx_fwd(flags);
    int hfd = mkostemp(t, hflags);
    if (hfd < 0) return -errno;
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) { close(hfd); return -EMFILE; }
    return wfd;
}

int32_t yos_mkostemps(struct yos_exec_ctx *ctx, uint32_t template_off,
                     int32_t suffixlen, int32_t flags)
{
    if (!template_off || template_off >= ctx->memory_size) return -EFAULT;
    char *t = (char *)(ctx->memory + template_off);
    extern int oflags_fb_to_lx_fwd(int);
    int hflags = oflags_fb_to_lx_fwd(flags);
    int hfd = mkostemps(t, suffixlen, hflags);
    if (hfd < 0) return -errno;
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) { close(hfd); return -EMFILE; }
    return wfd;
}

#if defined(__APPLE__)
/* Hand-rolled cv_stat_h2w for darwin: matches the FreeBSD i386
 * sysroot's struct stat layout that nvim/libuv was compiled against
 * (see build-darwin/sysroot/usr/include/sys/stat.h). The previous
 * version mirrored a stale 72-byte wasm32_stat (st_mode at offset 8),
 * which silently broke uv_guess_handle on every SOCK fd in the
 * embedded nvim server: libuv read st_mode=0 (the byte at the WRONG
 * offset), S_ISSOCK was false, and the IPC stdin was misclassified
 * as UV_FILE → never registered on kqueue → the embedded server hit
 * the "ch 1 was closed by the client" path within ~200 ms. The
 * auto-generated cv_stat_h2w on Linux already uses the layout below;
 * keeping the two in sync. */
#include <sys/stat.h>
static inline void cv_stat_h2w(uint8_t *w, const struct stat *h)
{
    /* FreeBSD i386 struct stat (192 bytes used; size with __spare is
     * 224, but the auto-gen layout stops at 192). */
    memset(w, 0, 192);
    *(int64_t *)(w +  0) = (int64_t)h->st_dev;
    *(int64_t *)(w +  8) = (int64_t)h->st_ino;
    *(int64_t *)(w + 16) = (int64_t)h->st_nlink;
    *(int16_t *)(w + 24) = (int16_t)h->st_mode;
    /* st_bsdflags @26: no host counterpart; left 0 */
    *(int32_t *)(w + 28) = (int32_t)h->st_uid;
    *(int32_t *)(w + 32) = (int32_t)h->st_gid;
    /* st_padding1 @36: 0 */
    *(int64_t *)(w + 40) = (int64_t)h->st_rdev;
    /* st_atim/mtim/ctim/birthtim @48..111: leave 0 (nvim's hot path
     * doesn't read them; the auto-gen converter on Linux also skips). */
    *(int64_t *)(w + 80) = (int64_t)h->st_size;
    *(int64_t *)(w + 88) = (int64_t)h->st_blocks;
    *(int32_t *)(w + 96) = (int32_t)h->st_blksize;
    *(int32_t *)(w + 100) = (int32_t)h->st_flags;
    *(int64_t *)(w + 104) = (int64_t)h->st_gen;
}
#else
extern void cv_stat_h2w(uint8_t *w, const struct stat *h);
#endif

int32_t yos_fstat(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t statbuf_off)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    struct stat h;
    memset(&h, 0, sizeof h);
    if (fstat(hfd, &h) < 0) return -errno;
    if (ydebug_enabled()) {
        const char *kind = "?";
        if (S_ISREG(h.st_mode))  kind = "REG";
        else if (S_ISDIR(h.st_mode)) kind = "DIR";
        else if (S_ISCHR(h.st_mode)) kind = "CHR/tty?";
        else if (S_ISBLK(h.st_mode)) kind = "BLK";
        else if (S_ISFIFO(h.st_mode)) kind = "FIFO";
        else if (S_ISSOCK(h.st_mode)) kind = "SOCK";
        ydebug("fstat(wfd=%d hfd=%d) mode=0%o (%s)\n",
               wfd, hfd, (unsigned)h.st_mode, kind);
    }
    cv_stat_h2w(ctx->memory + statbuf_off, &h);
    return 0;
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

/* issetugid: BSD/macOS only — Linux glibc doesn't ship it. Returns
 * non-zero if the process is running with elevated privileges that
 * make it unsafe to honour environment variables. Under yos a wasm
 * guest is always running as the user that launched it — never
 * privileged — so return 0. openssl's ossl_safe_getenv and openssh's
 * ssh_get_progname/etc. use this to decide whether to trust $PATH,
 * $HOME, etc. (we DO want them to trust). */
int32_t yos_issetugid(struct yos_exec_ctx *ctx) { (void)ctx; return 0; }

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

/* FreeBSD socket type flags:
 *   SOCK_CLOEXEC  = 0x10000000
 *   SOCK_NONBLOCK = 0x20000000
 * Linux socket type flags:
 *   SOCK_CLOEXEC  = 0x80000  (octal 02000000)
 *   SOCK_NONBLOCK = 0x800    (octal 04000)
 * Bottom byte (SOCK_STREAM=1, SOCK_DGRAM=2, …) matches.
 *
 * Without translation, libuv's `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
 * 0, sv)` fails with EINVAL on the Linux host — the spawn pipe is
 * never created — and every later `write()` to the (nonexistent) pipe
 * EPIPEs the RPC channel between the TUI parent and the embedded
 * server. nvim's TUI logs `chan_close_with_error: stream write
 * failed` and silently throws away every keystroke. */
static int sock_type_fb_to_lx(int t)
{
    int base = t & 0xff;       /* SOCK_STREAM/DGRAM/RAW/SEQPACKET — same. */
    int out = base;
    if (t & 0x10000000) out |= SOCK_CLOEXEC;
    if (t & 0x20000000) out |= SOCK_NONBLOCK;
    return out;
}
int sock_type_fb_to_lx_fwd(int t) { return sock_type_fb_to_lx(t); }

int32_t yos_socketpair(struct yos_exec_ctx *ctx, int32_t domain,
                       int32_t type, int32_t protocol, uint32_t sv_off)
{
    if (sv_off + 8 > ctx->memory_size) return -EFAULT;
    int hfds[2];
    /* Detect the FreeBSD high-bit flags before they get masked away
     * by sock_type_fb_to_lx (which on darwin maps both to 0 because
     * the platform has no SOCK_NONBLOCK/SOCK_CLOEXEC). Apply the
     * equivalent semantics via fcntl below. tmp/nvim-runtime-issues.md
     * covers why this matters for libuv's IPC channel. */
    int want_nonblock = !!(type & 0x20000000);
    int want_cloexec  = !!(type & 0x10000000);
    if (socketpair(domain, sock_type_fb_to_lx(type), protocol, hfds) < 0)
        return -errno;
    for (int i = 0; i < 2; i++) {
        if (want_nonblock) {
            int fl = fcntl(hfds[i], F_GETFL);
            if (fl >= 0) fcntl(hfds[i], F_SETFL, fl | O_NONBLOCK);
        }
        if (want_cloexec) {
            int fl = fcntl(hfds[i], F_GETFD);
            if (fl >= 0) fcntl(hfds[i], F_SETFD, fl | FD_CLOEXEC);
        }
    }
    int32_t wa = yos_fd_alloc(ctx, hfds[0]);
    if (wa < 0) { close(hfds[0]); close(hfds[1]); return -EMFILE; }
    int32_t wb = yos_fd_alloc(ctx, hfds[1]);
    if (wb < 0) { yos_fd_close(ctx, wa); close(hfds[1]); return -EMFILE; }
    int32_t *sv = (int32_t *)(ctx->memory + sv_off);
    sv[0] = wa;
    sv[1] = wb;
    return 0;
}

/* ── termios — FreeBSD wasm32 layout ↔ Linux glibc layout ──────────
 *
 * FreeBSD struct termios (44 B):
 *   c_iflag/oflag/cflag/lflag (4×4 B), c_cc[NCCS=20] @16, c_ispeed @36,
 *   c_ospeed @40
 * Linux glibc struct termios (60 B):
 *   c_iflag/oflag/cflag/lflag (4×4 B), c_line @16, c_cc[NCCS=32] @17,
 *   c_ispeed @52, c_ospeed @56
 *
 * Flag bits and control-character indices differ; we translate only
 * the ones nvim/libuv actually exercises (the four flag words plus
 * the c_cc subset that controls raw mode). Anything else stays
 * zero — that's safe for tcgetattr→tcsetattr round trips because
 * libuv only mutates a small subset before writing back. */

/* FreeBSD termios layout (offsets/sizes per
 * build-linux/src/yos/codegen/guest-api-i386-freebsd.yaml). 44 bytes. */
#define YOS_FBSD_TERMIOS_SIZE   44
#define YOS_FBSD_NCCS           20

/* FreeBSD c_cc[] indices (sys/_termios.h). */
enum { YOS_FB_VEOF=0, YOS_FB_VEOL=1, YOS_FB_VEOL2=2, YOS_FB_VERASE=3,
       YOS_FB_VWERASE=4, YOS_FB_VKILL=5, YOS_FB_VREPRINT=6,
       YOS_FB_VINTR=8, YOS_FB_VQUIT=9, YOS_FB_VSUSP=10, YOS_FB_VDSUSP=11,
       YOS_FB_VSTART=12, YOS_FB_VSTOP=13, YOS_FB_VLNEXT=14,
       YOS_FB_VDISCARD=15, YOS_FB_VMIN=16, YOS_FB_VTIME=17,
       YOS_FB_VSTATUS=18 };

/* FreeBSD c_iflag bits (sys/_termios.h). Names that match Linux's by
 * value are omitted; only divergent ones below.
 *   IGNBRK 0x0001, BRKINT 0x0002, IGNPAR 0x0004, PARMRK 0x0008,
 *   INPCK 0x0010, ISTRIP 0x0020, INLCR 0x0040, IGNCR 0x0080,
 *   ICRNL 0x0100, IXON 0x0200, IXOFF 0x0400, IXANY 0x0800,
 *   IMAXBEL 0x2000
 * Linux iflag (bits/termios-c_iflag.h):
 *   IGNBRK 0001, BRKINT 0002, IGNPAR 0004, PARMRK 0010,
 *   INPCK 0020, ISTRIP 0040, INLCR 0100, IGNCR 0200,
 *   ICRNL 0400, IUCLC 01000, IXON 02000, IXANY 04000,
 *   IXOFF 010000, IMAXBEL 020000
 * — first six bits identical, then offsets diverge. Same shape for
 *   the other three flag words. Translate via per-flag table. */

struct flag_map { uint32_t fb; uint32_t lx; };

static const struct flag_map iflag_map[] = {
    {0x0001, 0000001}, /* IGNBRK */
    {0x0002, 0000002}, /* BRKINT */
    {0x0004, 0000004}, /* IGNPAR */
    {0x0008, 0000010}, /* PARMRK */
    {0x0010, 0000020}, /* INPCK  */
    {0x0020, 0000040}, /* ISTRIP */
    {0x0040, 0000100}, /* INLCR  */
    {0x0080, 0000200}, /* IGNCR  */
    {0x0100, 0000400}, /* ICRNL  */
    {0x0200, 0002000}, /* IXON   */
    {0x0400, 0010000}, /* IXOFF  */
    {0x0800, 0004000}, /* IXANY  */
    {0x2000, 0020000}, /* IMAXBEL*/
};

static const struct flag_map oflag_map[] = {
    {0x0001, 0000001}, /* OPOST */
    {0x0002, 0000004}, /* ONLCR (FreeBSD 0x2 → Linux 0o4) */
    {0x0004, 0000040}, /* OXTABS (FreeBSD) → XTABS (Linux 040, mostly tab3) */
    {0x0008, 0000020}, /* ONOEOT (FreeBSD)/OFILL (Linux) — best-effort */
    {0x0010, 0000010}, /* OCRNL  */
    {0x0020, 0000100}, /* ONOCR  */
    {0x0040, 0000200}, /* ONLRET */
};

/* c_cflag — Linux uses CSIZE bits 060 (CS5..CS8 = 0/0o20/0o40/0o60),
 * FreeBSD uses 0x300 (CS5..CS8 = 0/0x100/0x200/0x300). */
static const struct flag_map cflag_map[] = {
    {0x0100, 0000020}, /* CS6 */
    {0x0200, 0000040}, /* CS7 */
    {0x0300, 0000060}, /* CS8 */
    {0x0400, 0000100}, /* CSTOPB */
    {0x0800, 0000200}, /* CREAD  */
    {0x1000, 0000400}, /* PARENB */
    {0x2000, 0001000}, /* PARODD */
    {0x4000, 0002000}, /* HUPCL  */
    {0x8000, 0004000}, /* CLOCAL */
};

/* c_lflag — divergent in BIT POSITION:
 *  FreeBSD: ECHOKE 0x1, ECHOE 0x2, ECHOK 0x4, ECHO 0x8, ECHONL 0x10,
 *           ECHOPRT 0x20, ECHOCTL 0x40, ISIG 0x80, ICANON 0x100,
 *           ALTWERASE 0x200, IEXTEN 0x400, EXTPROC 0x800, TOSTOP 0x400000,
 *           FLUSHO 0x800000, NOKERNINFO 0x2000000, PENDIN 0x20000000,
 *           NOFLSH 0x80000000
 *  Linux:   ISIG 0o1, ICANON 0o2, XCASE 0o4, ECHO 0o10, ECHOE 0o20,
 *           ECHOK 0o40, ECHONL 0o100, NOFLSH 0o200, TOSTOP 0o400,
 *           ECHOCTL 0o1000, ECHOPRT 0o2000, ECHOKE 0o4000, FLUSHO 0o10000,
 *           PENDIN 0o40000, IEXTEN 0o100000, EXTPROC 0o200000
 * The two ones nvim/libuv care about for raw mode are ISIG and ICANON
 * (cleared) and ECHO (cleared). */
static const struct flag_map lflag_map[] = {
    {0x00000001, 0004000}, /* ECHOKE */
    {0x00000002, 0000020}, /* ECHOE  */
    {0x00000004, 0000040}, /* ECHOK  */
    {0x00000008, 0000010}, /* ECHO   */
    {0x00000010, 0000100}, /* ECHONL */
    {0x00000020, 0002000}, /* ECHOPRT*/
    {0x00000040, 0001000}, /* ECHOCTL*/
    {0x00000080, 0000001}, /* ISIG   */
    {0x00000100, 0000002}, /* ICANON */
    {0x00000400, 0100000}, /* IEXTEN */
    {0x00000800, 0200000}, /* EXTPROC*/
    {0x00400000, 0000400}, /* TOSTOP */
    {0x00800000, 0010000}, /* FLUSHO */
    {0x20000000, 0040000}, /* PENDIN */
    {0x80000000u,0000200}, /* NOFLSH */
};

static uint32_t map_flags(uint32_t v, const struct flag_map *m, size_t n,
                          int fb_to_lx)
{
    uint32_t r = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t src = fb_to_lx ? m[i].fb : m[i].lx;
        uint32_t dst = fb_to_lx ? m[i].lx : m[i].fb;
        if (v & src) r |= dst;
    }
    return r;
}

/* c_cc[] index translation. fb_idx → linux_idx (or -1 if not on Linux). */
static int cc_fb_to_lx(int fb_idx)
{
    switch (fb_idx) {
    case YOS_FB_VEOF:    return VEOF;
    case YOS_FB_VEOL:    return VEOL;
    case YOS_FB_VEOL2:   return VEOL2;
    case YOS_FB_VERASE:  return VERASE;
    case YOS_FB_VWERASE: return VWERASE;
    case YOS_FB_VKILL:   return VKILL;
    case YOS_FB_VREPRINT:return VREPRINT;
    case YOS_FB_VINTR:   return VINTR;
    case YOS_FB_VQUIT:   return VQUIT;
    case YOS_FB_VSUSP:   return VSUSP;
    case YOS_FB_VSTART:  return VSTART;
    case YOS_FB_VSTOP:   return VSTOP;
    case YOS_FB_VLNEXT:  return VLNEXT;
    case YOS_FB_VDISCARD:return VDISCARD;
    case YOS_FB_VMIN:    return VMIN;
    case YOS_FB_VTIME:   return VTIME;
    default:             return -1;
    }
}

static void termios_fb_to_lx(struct termios *h, const uint8_t *w)
{
    uint32_t iflag = *(uint32_t *)(w +  0);
    uint32_t oflag = *(uint32_t *)(w +  4);
    uint32_t cflag = *(uint32_t *)(w +  8);
    uint32_t lflag = *(uint32_t *)(w + 12);
    const uint8_t *cc = w + 16;
    uint32_t ispeed = *(uint32_t *)(w + 36);
    uint32_t ospeed = *(uint32_t *)(w + 40);

    memset(h, 0, sizeof *h);
#if defined(__APPLE__) || defined(__FreeBSD__)
    /* darwin/FreeBSD host: termios flag bit positions match the
     * FreeBSD wasm guest verbatim (both BSD lineage). The map_flags
     * tables convert to LINUX positions, which on darwin would set
     * the wrong host bits — e.g. nvim's cfmakeraw clears ISIG (0x80
     * in FreeBSD) but the bridge would write Linux ISIG (0x1) to
     * the host, leaving darwin's ISIG (0x80) untouched and the pty
     * stuck in cooked mode. nvim's keystrokes then never reach the
     * read pipeline. Pass the flag words straight through; only the
     * c_cc[] index translation still applies because darwin's index
     * macros (VEOF/VINTR/VMIN/...) name the same numeric positions. */
    h->c_iflag = iflag;
    h->c_oflag = oflag;
    h->c_cflag = cflag;
    h->c_lflag = lflag;
#else
    h->c_iflag = map_flags(iflag, iflag_map,
                           sizeof iflag_map/sizeof iflag_map[0], 1);
    h->c_oflag = map_flags(oflag, oflag_map,
                           sizeof oflag_map/sizeof oflag_map[0], 1);
    /* CSIZE bits handled explicitly (CS5/CS6/CS7/CS8). */
    h->c_cflag = map_flags(cflag, cflag_map,
                           sizeof cflag_map/sizeof cflag_map[0], 1);
    h->c_lflag = map_flags(lflag, lflag_map,
                           sizeof lflag_map/sizeof lflag_map[0], 1);
#endif
    for (int i = 0; i < YOS_FBSD_NCCS; i++) {
        int li = cc_fb_to_lx(i);
        if (li >= 0 && li < NCCS) h->c_cc[li] = cc[i];
    }
    cfsetispeed(h, ispeed);
    cfsetospeed(h, ospeed);
}

static void termios_lx_to_fb(uint8_t *w, const struct termios *h)
{
    memset(w, 0, YOS_FBSD_TERMIOS_SIZE);
#if defined(__APPLE__) || defined(__FreeBSD__)
    /* See termios_fb_to_lx: identity passthrough on BSD-lineage
     * hosts; the bit positions already match the FreeBSD wasm guest's
     * expectations. Otherwise tcgetattr returns flags that, when
     * passed back through tcsetattr, get scrambled by the round-trip. */
    *(uint32_t *)(w +  0) = (uint32_t)h->c_iflag;
    *(uint32_t *)(w +  4) = (uint32_t)h->c_oflag;
    *(uint32_t *)(w +  8) = (uint32_t)h->c_cflag;
    *(uint32_t *)(w + 12) = (uint32_t)h->c_lflag;
#else
    *(uint32_t *)(w +  0) = map_flags(h->c_iflag, iflag_map,
                                      sizeof iflag_map/sizeof iflag_map[0], 0);
    *(uint32_t *)(w +  4) = map_flags(h->c_oflag, oflag_map,
                                      sizeof oflag_map/sizeof oflag_map[0], 0);
    *(uint32_t *)(w +  8) = map_flags(h->c_cflag, cflag_map,
                                      sizeof cflag_map/sizeof cflag_map[0], 0);
    *(uint32_t *)(w + 12) = map_flags(h->c_lflag, lflag_map,
                                      sizeof lflag_map/sizeof lflag_map[0], 0);
#endif
    uint8_t *cc = w + 16;
    for (int i = 0; i < YOS_FBSD_NCCS; i++) {
        int li = cc_fb_to_lx(i);
        if (li >= 0 && li < NCCS) cc[i] = h->c_cc[li];
    }
    *(uint32_t *)(w + 36) = (uint32_t)cfgetispeed(h);
    *(uint32_t *)(w + 40) = (uint32_t)cfgetospeed(h);
}

int32_t yos_tcgetattr(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t t_off)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return -EFAULT;
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    struct termios h;
    if (tcgetattr(hfd, &h) < 0) return -errno;
    termios_lx_to_fb(ctx->memory + t_off, &h);
    return 0;
}

int32_t yos_tcsetattr(struct yos_exec_ctx *ctx, int32_t wfd,
                      int32_t actions, uint32_t t_off)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return -EFAULT;
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    /* FreeBSD TCSANOW=0, TCSADRAIN=1, TCSAFLUSH=2 — same on Linux, no
     * remap needed. */
    struct termios h;
    /* Read current host state first so any flag bit we don't translate
     * is preserved across a guest tcgetattr+tcsetattr round trip. */
    if (tcgetattr(hfd, &h) < 0) return -errno;
    /* Now overlay the guest's desired flags. termios_fb_to_lx zeros
     * the host struct and writes the flags we know — the cfsetispeed
     * /cfsetospeed inside it pull in the speed bits. */
    termios_fb_to_lx(&h, ctx->memory + t_off);
    if (tcsetattr(hfd, actions, &h) < 0) return -errno;
    return 0;
}

/* cfmakeraw — operate directly on the wasm-side FreeBSD termios. The
 * auto-bridge passed it to host glibc which scrambles the 44-byte
 * struct because glibc writes Linux's 60-byte layout. */
void yos_cfmakeraw(struct yos_exec_ctx *ctx, uint32_t t_off)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return;
    uint8_t *w = ctx->memory + t_off;
    uint32_t iflag = *(uint32_t *)(w +  0);
    uint32_t oflag = *(uint32_t *)(w +  4);
    uint32_t cflag = *(uint32_t *)(w +  8);
    uint32_t lflag = *(uint32_t *)(w + 12);
    /* Match cfmakeraw(3) — clear flags that interpret input/output. */
    /* iflag: clear IGNBRK, BRKINT, PARMRK, ISTRIP, INLCR, IGNCR, ICRNL, IXON */
    iflag &= ~(uint32_t)(0x0001 | 0x0002 | 0x0008 | 0x0020 |
                         0x0040 | 0x0080 | 0x0100 | 0x0200);
    /* oflag: clear OPOST */
    oflag &= ~(uint32_t)0x0001;
    /* lflag: clear ECHO, ECHONL, ICANON, ISIG, IEXTEN */
    lflag &= ~(uint32_t)(0x0008 | 0x0010 | 0x0100 | 0x0080 | 0x0400);
    /* cflag: clear CSIZE | PARENB, set CS8 */
    cflag &= ~(uint32_t)(0x0300 | 0x1000);
    cflag |=  (uint32_t)0x0300;  /* CS8 */
    *(uint32_t *)(w +  0) = iflag;
    *(uint32_t *)(w +  4) = oflag;
    *(uint32_t *)(w +  8) = cflag;
    *(uint32_t *)(w + 12) = lflag;
    /* c_cc: VMIN=16, VTIME=17 in FreeBSD. */
    w[16 + 16] = 1;  /* VMIN */
    w[16 + 17] = 0;  /* VTIME */
}

int32_t yos_cfsetispeed(struct yos_exec_ctx *ctx, uint32_t t_off, uint32_t speed)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return -EFAULT;
    *(uint32_t *)(ctx->memory + t_off + 36) = speed;
    return 0;
}

int32_t yos_cfsetospeed(struct yos_exec_ctx *ctx, uint32_t t_off, uint32_t speed)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return -EFAULT;
    *(uint32_t *)(ctx->memory + t_off + 40) = speed;
    return 0;
}

uint32_t yos_cfgetispeed(struct yos_exec_ctx *ctx, uint32_t t_off)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return 0;
    return *(uint32_t *)(ctx->memory + t_off + 36);
}

uint32_t yos_cfgetospeed(struct yos_exec_ctx *ctx, uint32_t t_off)
{
    if (t_off + YOS_FBSD_TERMIOS_SIZE > ctx->memory_size) return 0;
    return *(uint32_t *)(ctx->memory + t_off + 40);
}

/* ── utimes / futimes / lutimes ──────────────────────────────────────
 *
 * struct timeval is { time_t tv_sec; suseconds_t tv_usec; }. On
 * FreeBSD-i386 wasm32 both fields are 32-bit (8 bytes total). On host
 * x86_64 Linux tv_sec is 64-bit (16 bytes total). The wasm guest
 * passes a wasm offset to a 2-element timeval array (16 bytes); we
 * widen each scalar to host shape before calling host libc.
 *
 * Auto-bridge stubs these because struct timeval is in the
 * struct_convert table for in/out scalar fields but not for the
 * 2-element array form these fns use. Hand-bridge: read 4 little-
 * endian uint32s from wasm, build 2 host timevals, call.
 */
#include <sys/time.h>

static void wasm_timeval2_to_host(const uint8_t *w, struct timeval h[2])
{
    for (int i = 0; i < 2; i++) {
        uint32_t s, u;
        memcpy(&s, w + i*8 + 0, 4);
        memcpy(&u, w + i*8 + 4, 4);
        h[i].tv_sec  = (time_t)(int32_t)s;   /* sign-extend */
        h[i].tv_usec = (suseconds_t)(int32_t)u;
    }
}

int32_t yos_utimes(struct yos_exec_ctx *ctx, uint32_t path_off, uint32_t times_off)
{
    if (!path_off || path_off >= ctx->memory_size) return -EFAULT;
    const char *path = (const char *)(ctx->memory + path_off);
    struct timeval *p = NULL, h[2];
    if (times_off) {
        if (times_off + 16 > ctx->memory_size) return -EFAULT;
        wasm_timeval2_to_host(ctx->memory + times_off, h);
        p = h;
    }
    if (utimes(path, p) < 0) return -errno;
    return 0;
}

int32_t yos_futimes(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t times_off)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    struct timeval *p = NULL, h[2];
    if (times_off) {
        if (times_off + 16 > ctx->memory_size) return -EFAULT;
        wasm_timeval2_to_host(ctx->memory + times_off, h);
        p = h;
    }
    if (futimes(hfd, p) < 0) return -errno;
    return 0;
}

int32_t yos_lutimes(struct yos_exec_ctx *ctx, uint32_t path_off, uint32_t times_off)
{
    if (!path_off || path_off >= ctx->memory_size) return -EFAULT;
    const char *path = (const char *)(ctx->memory + path_off);
    struct timeval *p = NULL, h[2];
    if (times_off) {
        if (times_off + 16 > ctx->memory_size) return -EFAULT;
        wasm_timeval2_to_host(ctx->memory + times_off, h);
        p = h;
    }
    if (lutimes(path, p) < 0) return -errno;
    return 0;
}

/* __xuname — FreeBSD's uname(2) backend. The inline `uname()` in
 * <sys/utsname.h> calls __xuname(SYS_NMLN=256, struct utsname*).
 * struct utsname on wasm is 5 fields × 256 bytes = 1280 bytes.
 *
 * We pretend to be FreeBSD on wasm32 (the wasm guest's whole point).
 * sysname=FreeBSD, machine=wasm32. Rest is just pleasant defaults
 * pulled from host uname so build scripts get a sensible kernel
 * version when they grep release. */
#include <sys/utsname.h>
int32_t yos___xuname(struct yos_exec_ctx *ctx, int32_t namesz, uint32_t buf_off)
{
    if (namesz <= 0 || !buf_off) return -EFAULT;
    uint32_t total = (uint32_t)namesz * 5;
    if (buf_off + total > ctx->memory_size) return -EFAULT;
    char *fields[5] = {
        (char *)(ctx->memory + buf_off + 0u * (uint32_t)namesz),  /* sysname */
        (char *)(ctx->memory + buf_off + 1u * (uint32_t)namesz),  /* nodename */
        (char *)(ctx->memory + buf_off + 2u * (uint32_t)namesz),  /* release */
        (char *)(ctx->memory + buf_off + 3u * (uint32_t)namesz),  /* version */
        (char *)(ctx->memory + buf_off + 4u * (uint32_t)namesz),  /* machine */
    };
    /* Zero everything first so any short string is NUL-terminated. */
    memset(ctx->memory + buf_off, 0, total);

    struct utsname host;
    if (uname(&host) < 0) return -errno;

    /* Present a FreeBSD wasm32 face regardless of host. */
    strncpy(fields[0], "FreeBSD", (size_t)namesz - 1);
    /* nodename = host's hostname (real). */
    strncpy(fields[1], host.nodename, (size_t)namesz - 1);
    /* release = "14.4-yos" so build scripts that test FreeBSD>=12 work. */
    strncpy(fields[2], "14.4-yos", (size_t)namesz - 1);
    /* version = a free-form build-time string. */
    snprintf(fields[3], (size_t)namesz, "FreeBSD 14.4-yos wasm32 host=%s",
             host.release);
    /* machine = wasm32. */
    strncpy(fields[4], "wasm32", (size_t)namesz - 1);
    return 0;
}

/* if_indextoname — return a wasm offset to a buffer holding the
 * interface name. Auto-bridge stubbed because the return value
 * `char *` aliases the second arg. We let host libc fill the
 * caller's buffer and return its wasm offset on success. */
#include <net/if.h>
uint32_t yos_if_indextoname(struct yos_exec_ctx *ctx,
                            uint32_t ifindex, uint32_t name_off)
{
    if (!name_off || name_off + IF_NAMESIZE > ctx->memory_size) return 0;
    char *buf = (char *)(ctx->memory + name_off);
    if (!if_indextoname(ifindex, buf)) return 0;
    return name_off;
}

/* accept4 — like accept(2) but with flags. Same fd virtualisation
 * as accept; FreeBSD SOCK_CLOEXEC etc. translate to host equivalents.
 * Auto-bridge classified as "unportable Linux extension"; on Linux it
 * IS available (glibc 2.10+). Stub on darwin until a fcntl-based
 * fallback lands. */
int32_t yos_accept4(struct yos_exec_ctx *ctx, int32_t wfd,
                    uint32_t addr_off, uint32_t addrlen_off, int32_t flags)
{
#if defined(__linux__)
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return -EBADF;
    /* Strip FreeBSD-specific bits we can't honour atomically and
     * convert to host equivalents. */
    int hflags = 0;
    if (flags & 0x10000000) hflags |= SOCK_CLOEXEC;   /* FreeBSD SOCK_CLOEXEC */
    if (flags & 0x20000000) hflags |= SOCK_NONBLOCK;  /* FreeBSD SOCK_NONBLOCK */
    /* Auto-bridge can't translate sockaddr; we accept into a host
     * buffer and copy back, mirroring yos_accept's pattern in the
     * generated code. For minimum portability, ignore the addr
     * out-param if non-NULL — caller can getpeername later. */
    int new_hfd = accept4(hfd, NULL, NULL, hflags);
    (void)addr_off; (void)addrlen_off;
    if (new_hfd < 0) return -errno;
    int new_wfd = yos_fd_alloc(ctx, new_hfd);
    if (new_wfd < 0) { close(new_hfd); return -EMFILE; }
    return new_wfd;
#else
    (void)ctx; (void)wfd; (void)addr_off; (void)addrlen_off; (void)flags;
    return -ENOSYS;
#endif
}
