/* Minimal <poll.h> shim for MinGW-w64.
 *
 * Provides struct pollfd / POLLIN / poll(). Backed by WSAPoll for
 * socket fds. ydev callers use it on socketpair-backed self-pipes
 * (created via the yos_winshim.h pipe() rewrite), where WSAPoll is
 * the right thing. */
#ifndef YOS_WIN_COMPAT_POLL_H
#define YOS_WIN_COMPAT_POLL_H

#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#ifndef POLLIN
#define POLLIN      0x0001
#endif
#ifndef POLLOUT
#define POLLOUT     0x0004
#endif
#ifndef POLLERR
#define POLLERR     0x0008
#endif
#ifndef POLLHUP
#define POLLHUP     0x0010
#endif
#ifndef POLLNVAL
#define POLLNVAL    0x0020
#endif

/* MinGW's winsock2.h declares `struct pollfd` whenever
 * _WIN32_WINNT >= 0x600 (the default on modern MinGW). We pull
 * winsock2 above so the struct is in scope here; no redeclaration. */

typedef unsigned long nfds_t;

/* Real body in compat_libc.c — handles a mix of Winsock SOCKETs and
 * CRT file descriptors / pipes by routing each side appropriately. */
extern int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* YOS_WIN_COMPAT_POLL_H */
