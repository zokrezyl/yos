/* impl/libc/posix-windows.c — Windows host POSIX bridges.
 *
 * MinGW-w64 / msvcrt ship a partial POSIX surface; Windows native
 * sockets via winsock2 use a Linux-style two-byte sa_family at offset
 * 0 (no sa_len byte), so we use the Linux strip-sa_len behaviour.
 *
 * termios: Windows console mode is not termios. Wasm callers that
 * need cooked-mode toggling won't get the real thing here; we keep
 * the byte-shuffling layout-compatible with the FreeBSD wasm side so
 * tcgetattr/tcsetattr round-trip the data but the host kernel never
 * sees it. termios_fb_to_lx zeroes the host struct; termios_lx_to_fb
 * reads back what little we have from a scratch all-zero state.
 *
 * accept4 is a Linux-ism; on Windows we emulate the NONBLOCK flag via
 * ioctlsocket FIONBIO after a plain accept().
 *
 * NO #ifdef in this file — meson selects it only on windows hosts.
 */

#include "yos/types.h"
#include "impl/errno_helpers.h"
#include "impl/libc/posix-internal.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

extern int32_t yos_fd_get  (struct yos_exec_ctx *ctx, int32_t wfd);
extern int32_t yos_fd_alloc(struct yos_exec_ctx *ctx, int host_fd);

uint16_t read_host_sa_family(const uint8_t *host_buf)
{
    /* Winsock: same as Linux — uint16 little-endian at offset 0|1. */
    return (uint16_t)(host_buf[0] | (host_buf[1] << 8));
}

void freebsd_sockaddr_to_host(uint8_t *buf, socklen_t len)
{
    if (len >= 2) {
        uint8_t sa_family_byte = buf[1];
        buf[0] = sa_family_byte;
        buf[1] = 0;
    }
}

/* MinGW provides struct termios in <termios.h> via pthread / mintty
 * shims sometimes, but the safe assumption for a pure Windows build
 * is that termios is unavailable. The codegen-produced posix.c
 * declares the converter signatures with `struct termios *` from the
 * host's <termios.h>; we therefore avoid touching it here and accept
 * the converters operate on opaque memory. */
#define YOS_FBSD_NCCS         20
#define YOS_FBSD_TERMIOS_SIZE 44

void termios_fb_to_lx(struct termios *h, const uint8_t *w)
{
    (void)w;
    memset(h, 0, sizeof *h);
}

void termios_lx_to_fb(uint8_t *w, const struct termios *h)
{
    (void)h;
    memset(w, 0, YOS_FBSD_TERMIOS_SIZE);
}

int32_t yos_accept4(struct yos_exec_ctx *ctx, int32_t wfd,
                    uint32_t addr_off, uint32_t addrlen_off, int32_t flags)
{
    (void)addr_off; (void)addrlen_off;
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_neg(ctx, EBADF);

    SOCKET hsock = (SOCKET)hfd;
    SOCKET new_sock = accept(hsock, NULL, NULL);
    if (new_sock == INVALID_SOCKET) return yos_errno_neg(ctx, EINVAL);

    if (flags & 0x20000000 /* FreeBSD SOCK_NONBLOCK */) {
        u_long nb = 1;
        ioctlsocket(new_sock, FIONBIO, &nb);
    }
    /* SOCK_CLOEXEC is Linux-only; Windows handle inheritance is
     * controlled per-CreateProcess via STARTUPINFO, so we drop the
     * bit silently. */

    int new_wfd = yos_fd_alloc(ctx, (int)new_sock);
    if (new_wfd < 0) { closesocket(new_sock); return yos_errno_neg(ctx, EMFILE); }
    return new_wfd;
}
