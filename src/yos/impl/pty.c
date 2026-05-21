/* impl/pty.c — pseudo-terminal bridges with sandbox fallback.
 *
 * On Linux/macOS desktop, posix_openpt() opens /dev/ptmx and returns
 * a real PTY master. Subsequent grantpt + unlockpt + ptsname all
 * work against that fd, and open(ptsname(master)) yields the slave.
 *
 * tvOS / iOS app sandboxes deny open("/dev/ptmx") with EPERM, so the
 * real call collapses; telnetd then prints "All network ports in
 * use." and exits before any traffic flows. Emulate the chain with
 * an AF_UNIX SOCK_STREAM socketpair:
 *
 *   master_hfd = sp[0], slave_hfd = sp[1]
 *   ptsname(master_hfd) → "/dev/yos-pts/N"  (synthesised path)
 *   open("/dev/yos-pts/N") → dup(slave_hfd) (via yos_pty_try_open)
 *
 * Master/slave then exchange bytes through the socketpair the same
 * way they would through a real PTY's character device. login_tty()
 * on the slave dups it onto stdin/stdout/stderr; the controlling-tty
 * ioctl is virtualised in vfs.c so the no-tty hardware doesn't fail
 * the chain.
 *
 * Limitations:
 *   - No line discipline. tcgetattr/tcsetattr on a socket return
 *     ENOTTY. zsh runs in raw-input mode — fine for telnetd which
 *     does its own IAC echo / line negotiation.
 *   - TIOCGWINSZ / TIOCSWINSZ return EINVAL on the socket. Apps
 *     default to 80x24.
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "yos/types.h"
#include <yos/ytrace/ytrace.h>
#include "impl/errno_helpers.h"

extern int yos_fd_alloc(struct yos_exec_ctx *, int);
extern int yos_fd_get  (struct yos_exec_ctx *, int);

/* Fake-PTY registry. Keyed on master_hfd value AND master inode/dev
 * (via fstat) so subsequent ptsname / ptsname_r calls match even when
 * the caller asks via a dup of master. */
struct pty_entry {
    int   master_hfd;
    ino_t master_ino;
    dev_t master_dev;
    int   slave_hfd;
    char *slave_path;     /* "/dev/yos-pts/N" */
    struct pty_entry *next;
};

static pthread_mutex_t g_pty_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pty_entry *g_pty_head;
static int g_pty_next_id = 1;

/* Match by master fd's inode/dev so multiple host dups of the same
 * master socket resolve to the same entry. */
static struct pty_entry *pty_lookup_by_master_locked(int master_hfd) {
    struct stat ms;
    if (fstat(master_hfd, &ms) < 0) return NULL;
    for (struct pty_entry *e = g_pty_head; e; e = e->next)
        if (e->master_ino == ms.st_ino && e->master_dev == ms.st_dev)
            return e;
    return NULL;
}

static struct pty_entry *pty_lookup_by_path_locked(const char *path) {
    for (struct pty_entry *e = g_pty_head; e; e = e->next)
        if (strcmp(e->slave_path, path) == 0) return e;
    return NULL;
}

/* Called from yos_open: returns a fresh dup of the slave end on hit,
 * -1 with errno=0 on miss (caller falls through to host open). */
int yos_pty_try_open(const char *path, int flags) {
    if (strncmp(path, "/dev/yos-pts/", 13) != 0) {
        errno = 0;
        return -1;
    }
    pthread_mutex_lock(&g_pty_lock);
    struct pty_entry *e = pty_lookup_by_path_locked(path);
    if (!e) { pthread_mutex_unlock(&g_pty_lock); errno = ENOENT; return -1; }
    int dupcmd = (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD;
    int dup = fcntl(e->slave_hfd, dupcmd, 0);
    pthread_mutex_unlock(&g_pty_lock);
    return dup;
}

int32_t yos_posix_openpt(struct yos_exec_ctx *ctx, int32_t fb_flags)
{
    int host_flags = O_RDWR;
    if (fb_flags & 0x8000) host_flags |= O_NOCTTY;
    int hfd = posix_openpt(host_flags);
    if (hfd >= 0) {
        int wfd = yos_fd_alloc(ctx, hfd);
        if (wfd < 0) { close(hfd); return yos_errno_neg(ctx, ENFILE); }
        ydebug("posix_openpt(0x%x→0x%x) = wfd=%d hfd=%d (real)\n",
               fb_flags, host_flags, wfd, hfd);
        return wfd;
    }
    if (errno != EPERM && errno != EACCES && errno != ENOENT)
        return yos_errno_neg(ctx, errno);

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return yos_errno_neg(ctx, errno);

    struct pty_entry *e = (struct pty_entry *)calloc(1, sizeof *e);
    if (!e) { close(sp[0]); close(sp[1]); return yos_errno_neg(ctx, ENOMEM); }

    struct stat ms;
    if (fstat(sp[0], &ms) < 0) {
        int saved = errno;
        free(e); close(sp[0]); close(sp[1]);
        return yos_errno_neg(ctx, saved);
    }
    e->master_hfd = sp[0];
    e->master_ino = ms.st_ino;
    e->master_dev = ms.st_dev;
    e->slave_hfd  = sp[1];

    pthread_mutex_lock(&g_pty_lock);
    int id = g_pty_next_id++;
    pthread_mutex_unlock(&g_pty_lock);
    char buf[64];
    snprintf(buf, sizeof buf, "/dev/yos-pts/%d", id);
    e->slave_path = strdup(buf);

    pthread_mutex_lock(&g_pty_lock);
    e->next = g_pty_head;
    g_pty_head = e;
    pthread_mutex_unlock(&g_pty_lock);

    int wfd = yos_fd_alloc(ctx, sp[0]);
    if (wfd < 0) { close(sp[0]); return yos_errno_neg(ctx, ENFILE); }
    ydebug("posix_openpt(0x%x→0x%x) = wfd=%d hfd=%d (fake, slave=%s)\n",
           fb_flags, host_flags, wfd, sp[0], e->slave_path);
    return wfd;
}

int32_t yos_grantpt(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_neg(ctx, EBADF);
    int rc = grantpt(hfd);
    if (rc == 0) return 0;
    /* On the socketpair-emulated master, grantpt returns ENOTTY. We
     * silently accept — there's nothing to grant on a socket. */
    if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP ||
        errno == ENOTSUP || errno == ENOTSOCK) {
        ydebug("grantpt(hfd=%d) = 0 (fake)\n", hfd);
        return 0;
    }
    return yos_errno_neg(ctx, errno);
}

int32_t yos_unlockpt(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_neg(ctx, EBADF);
    int rc = unlockpt(hfd);
    if (rc == 0) return 0;
    if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP ||
        errno == ENOTSUP || errno == ENOTSOCK) {
        ydebug("unlockpt(hfd=%d) = 0 (fake)\n", hfd);
        return 0;
    }
    return yos_errno_neg(ctx, errno);
}

#define YOS_PTSNAME_BUF 256
uint32_t yos_ptsname(struct yos_exec_ctx *ctx, int32_t wfd)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return yos_errno_null(ctx, EBADF);

    char *hs = ptsname(hfd);
    char fake_path[YOS_PTSNAME_BUF];
    if (!hs) {
        pthread_mutex_lock(&g_pty_lock);
        struct pty_entry *e = pty_lookup_by_master_locked(hfd);
        if (e) {
            strncpy(fake_path, e->slave_path, sizeof fake_path - 1);
            fake_path[sizeof fake_path - 1] = 0;
            hs = fake_path;
        }
        pthread_mutex_unlock(&g_pty_lock);
    }
    if (!hs) return yos_errno_null(ctx, ENOTTY);

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
    if (rc == 0) return 0;
    /* Fake fallback. */
    pthread_mutex_lock(&g_pty_lock);
    struct pty_entry *e = pty_lookup_by_master_locked(hfd);
    if (e) {
        size_t n = strlen(e->slave_path);
        if (n >= buflen) { pthread_mutex_unlock(&g_pty_lock); return ERANGE; }
        memcpy(guest, e->slave_path, n + 1);
        pthread_mutex_unlock(&g_pty_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_pty_lock);
    return ENOTTY;
}
