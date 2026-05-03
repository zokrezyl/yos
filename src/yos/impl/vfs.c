#define _GNU_SOURCE
#include "yos/types.h"
#include "yos/ydebug.h"
#include "impl/errno_helpers.h"
#include "vfs/mount.h"
#include "vfs/file.h"
#include "vfs/procfs.h"
#include "wasm32_structs.h"
#include "host64_structs.h"
#include "struct_convert.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <linux/stat.h>  /* for struct statx */
#include <linux/time_types.h> /* struct __kernel_timespec */
#include <sys/ioctl.h>
#include <dirent.h>
#include <stdio.h>

/* helper: wasm32 pointer to host pointer */
static inline void *wptr(struct yos_exec_ctx *ctx, uint32_t offset)
{
    if (offset == 0 || offset >= ctx->memory_size) return 0;
    return ctx->memory + offset;
}

/* helper: wasm32 string to host string (just a pointer into wasm memory) */
static inline const char *wstr(struct yos_exec_ctx *ctx, uint32_t offset)
{
    return (const char *)wptr(ctx, offset);
}

/* ============================================================================
 * Per-runtime fd table.
 * ============================================================================ */

void yos_fd_table_init(struct yos_exec_ctx *ctx)
{
    for (int i = 0; i < YOS_FD_MAX; i++) ctx->fd_map[i] = -1;
    /* Inherit yos's stdio at the conventional positions. */
    ctx->fd_map[0] = 0;
    ctx->fd_map[1] = 1;
    ctx->fd_map[2] = 2;
}

int32_t yos_fd_get(struct yos_exec_ctx *ctx, int32_t wfd)
{
    if (wfd == AT_FDCWD) return wfd;
    if (wfd < 0 || wfd >= YOS_FD_MAX) return -EBADF;
    int hfd = ctx->fd_map[wfd];
    if (hfd < 0) return -EBADF;
    return hfd;
}

int32_t yos_fd_alloc(struct yos_exec_ctx *ctx, int host_fd)
{
    if (host_fd < 0) return host_fd;
    for (int i = 0; i < YOS_FD_MAX; i++) {
        if (ctx->fd_map[i] < 0) {
            ctx->fd_map[i] = host_fd;
            return i;
        }
    }
    close(host_fd);
    return -EMFILE;
}

int32_t yos_fd_assign(struct yos_exec_ctx *ctx, int32_t newfd, int host_fd)
{
    if (host_fd < 0) return host_fd;
    if (newfd < 0 || newfd >= YOS_FD_MAX) {
        close(host_fd);
        return -EBADF;
    }
    int old = ctx->fd_map[newfd];
    if (old >= 0 && old != host_fd)
        close(old);
    ctx->fd_map[newfd] = host_fd;
    return newfd;
}

int32_t yos_fd_close(struct yos_exec_ctx *ctx, int32_t wfd)
{
    if (wfd < 0 || wfd >= YOS_FD_MAX) return -EBADF;
    int hfd = ctx->fd_map[wfd];
    if (hfd < 0) return -EBADF;
    int r = close(hfd);
    ctx->fd_map[wfd] = -1;
    return r < 0 ? -errno : 0;
}

void yos_fd_fork_dup(struct yos_exec_ctx *child, struct yos_exec_ctx *parent)
{
    for (int i = 0; i < YOS_FD_MAX; i++) {
        int phfd = parent->fd_map[i];
        if (phfd < 0) {
            child->fd_map[i] = -1;
            continue;
        }
        /* POSIX fork preserves FD_CLOEXEC; F_DUPFD strips it. Use
         * F_DUPFD_CLOEXEC when the source has it set so subsequent
         * exec()s correctly close the descriptor — without this,
         * libuv's spawn signal pipe (CLOEXEC) survives our pseudo-
         * exec, the post-execvp failure path in the child writes
         * errno into it, and the parent treats the spawn as failed. */
    int flags = fcntl(phfd, F_GETFD);
    int dupcmd = (flags >= 0 && (flags & FD_CLOEXEC)) ? F_DUPFD_CLOEXEC
                                                          : F_DUPFD;
        int chfd = fcntl(phfd, dupcmd, 0);
        child->fd_map[i] = (chfd >= 0) ? chfd : -1;
    }
}

/* Back-compat shim: code paths still calling yos_fd_translate / host_fd
 * route through the new table. */
int32_t yos_fd_translate(struct yos_exec_ctx *ctx, int32_t fd)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    return hfd < 0 ? fd : hfd;
}

static inline int32_t host_fd(struct yos_exec_ctx *ctx, int32_t fd)
{
    return yos_fd_translate(ctx, fd);
}

int32_t yos_read(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf, uint32_t count)
{
    void *p = wptr(ctx, buf);
    if (!p) return -EFAULT;

    /* Check if virtual fd */
    if (yos_is_virtual_fd(fd)) {
        struct yos_file_table *ft = (struct yos_file_table *)ctx->procfs_fds;
        if (!ft) return -EBADF;
        struct yos_file *file = yos_file_get(ft, fd);
        if (!file) return -EBADF;
        if (file->ops && file->ops->read)
            return file->ops->read(ctx, file, p, count);
        return -EBADF;
    }

    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    ssize_t r = read(hfd, p, count);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_write(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf, uint32_t count)
{
    void *p = wptr(ctx, buf);
    if (!p) return -EFAULT;
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    /* DEBUG: dump nvim's vim._init_packages module bytes (linear memory
     * 0x6aa70) on every write so we see corruption timeline. */
    if (getenv("YOS_DUMP_INIT")) {
        const uint8_t *m = ctx->memory + 0x6aa70;
        fprintf(stderr, "yos: dump@0x6aa70: %02x %02x %02x %02x %02x %02x \"%.20s\"\n",
                m[0], m[1], m[2], m[3], m[4], m[5], (const char *)m);
    }
    ssize_t r = write(hfd, p, count);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_open(struct yos_exec_ctx *ctx, uint32_t path, int32_t flags, int32_t mode)
{
    const char *s = wstr(ctx, path);
    if (!s) return -EFAULT;

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, s, &remaining);
        if (ops && ops->open) {
            return ops->open(ctx, remaining, flags, mode);
        }
    }

    int r = open(s, flags, mode);
    if (r < 0) return -errno;
    return yos_fd_alloc(ctx, r);
}

int32_t yos_close(struct yos_exec_ctx *ctx, int32_t fd)
{
    /* Check if virtual fd */
    if (yos_is_virtual_fd(fd)) {
        struct yos_file_table *ft = (struct yos_file_table *)ctx->procfs_fds;
        if (!ft) return -EBADF;
        struct yos_file *file = yos_file_get(ft, fd);
        if (!file) return -EBADF;
        int32_t ret = 0;
        if (file->ops && file->ops->close)
            ret = file->ops->close(ctx, file);
        yos_file_free(ft, fd);
        return ret;
    }

    return yos_fd_close(ctx, fd);
}

int32_t yos_creat(struct yos_exec_ctx *ctx, uint32_t pathname, int32_t mode)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return -EFAULT;
    int r = creat(s, mode);
    if (r < 0) return -errno;
    return yos_fd_alloc(ctx, r);
}

int32_t yos_link(struct yos_exec_ctx *ctx, uint32_t oldname, uint32_t newname)
{
    const char *o = wstr(ctx, oldname);
    const char *n = wstr(ctx, newname);
    if (!o || !n) return -EFAULT;
    return link(o, n) < 0 ? -errno : 0;
}

int32_t yos_unlink(struct yos_exec_ctx *ctx, uint32_t pathname)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return yos_errno_neg(ctx, EFAULT);
    return yos_errno_check(ctx, unlink(s));
}

int32_t yos_chdir(struct yos_exec_ctx *ctx, uint32_t filename)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;

    if (chdir(s) < 0)
        return -errno;

    /* Track cwd - resolve to absolute path */
    if (s[0] == '/') {
        /* Absolute path */
        strncpy(ctx->cwd, s, PATH_MAX - 1);
        ctx->cwd[PATH_MAX - 1] = '\0';
    } else {
        /* Relative path - append to current cwd */
        size_t cwdlen = strlen(ctx->cwd);
        if (cwdlen > 0 && ctx->cwd[cwdlen - 1] != '/')
            strncat(ctx->cwd, "/", PATH_MAX - cwdlen - 1);
        strncat(ctx->cwd, s, PATH_MAX - strlen(ctx->cwd) - 1);
    }
    /* TODO: canonicalize path (resolve . and ..) */

    return 0;
}

int32_t yos_chmod(struct yos_exec_ctx *ctx, uint32_t filename, int32_t mode)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return chmod(s, mode) < 0 ? -errno : 0;
}

int32_t yos_lchown(struct yos_exec_ctx *ctx, uint32_t filename, int32_t user, int32_t group)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return lchown(s, user, group) < 0 ? -errno : 0;
}

int32_t yos_lseek(struct yos_exec_ctx *ctx, int32_t fd, int32_t offset, int32_t whence)
{
    (void)ctx;
    off_t r = lseek(host_fd(ctx, fd), offset, whence);
    return r < 0 ? -errno : (int32_t)r;
}

/* _llseek: 64-bit seek for 32-bit systems
 * Combines offset_high/offset_low into 64-bit offset, writes result to *result_ptr */
int32_t yos_vfs__llseek(struct yos_exec_ctx *ctx, int32_t fd, uint32_t offset_high,
                        uint32_t offset_low, uint32_t result_ptr, int32_t whence)
{
    int64_t *result = wptr(ctx, result_ptr);
    if (!result) return -EFAULT;

    off_t offset = ((off_t)offset_high << 32) | offset_low;
    off_t r = lseek(host_fd(ctx, fd), offset, whence);
    if (r < 0) {
        return -errno;
    }
    *result = r;
    return 0;
}

int32_t yos_access(struct yos_exec_ctx *ctx, uint32_t filename, int32_t mode)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return access(s, mode) < 0 ? -errno : 0;
}

int32_t yos_rename(struct yos_exec_ctx *ctx, uint32_t oldname, uint32_t newname)
{
    const char *o = wstr(ctx, oldname);
    const char *n = wstr(ctx, newname);
    if (!o || !n) return -EFAULT;
    return rename(o, n) < 0 ? -errno : 0;
}

int32_t yos_mkdir(struct yos_exec_ctx *ctx, uint32_t pathname, int32_t mode)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return -EFAULT;
    return mkdir(s, mode) < 0 ? -errno : 0;
}

int32_t yos_rmdir(struct yos_exec_ctx *ctx, uint32_t pathname)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return -EFAULT;
    return rmdir(s) < 0 ? -errno : 0;
}

int32_t yos_pipe(struct yos_exec_ctx *ctx, uint32_t fildes)
{
    int *p = wptr(ctx, fildes);
    if (!p) return -EFAULT;
    int hfds[2];
    if (pipe(hfds) < 0) return -errno;
    int32_t r = yos_fd_alloc(ctx, hfds[0]);
    if (r < 0) { close(hfds[1]); return r; }
    int32_t w = yos_fd_alloc(ctx, hfds[1]);
    if (w < 0) { yos_fd_close(ctx, r); return w; }
    p[0] = r;
    p[1] = w;
    return 0;
}

int32_t yos_ioctl(struct yos_exec_ctx *ctx, int32_t fd, uint32_t cmd, uint32_t arg)
{
    /* Common ioctl commands that need handling */
    /* TIOCGWINSZ = 0x5413 - get window size */
    /* TIOCSWINSZ = 0x5414 - set window size */
    /* TCGETS = 0x5401 - get terminal attributes */
    /* TCSETS = 0x5402 - set terminal attributes */

    ydebug("ioctl(fd=%d, cmd=0x%x, arg=0x%x)\n", fd, cmd, arg);

    int hfd = host_fd(ctx, fd);
    void *argp = arg ? wptr(ctx, arg) : NULL;

    /* Virtualize controlling-tty foreground pgrp queries/sets so the
     * pgid the wasm caller stores/reads belongs to the *guest* pid
     * namespace (matching getpgrp(), setpgid()) rather than the host
     * shell's pgrp the kernel would report. Only intercept on tty fds
     * — non-tty TIOCGPGRP/TIOCSPGRP would just fail with ENOTTY which
     * is the correct kernel behavior. */
    if ((cmd == 0x540F /*TIOCGPGRP*/ || cmd == 0x5410 /*TIOCSPGRP*/)
        && hfd >= 0 && isatty(hfd)) {
        if (!argp) return -EFAULT;
        if (cmd == 0x540F) {
            *(int32_t *)argp = ctx->rt->fg_pgid;
            ydebug("ioctl TIOCGPGRP(virt) = %d\n", ctx->rt->fg_pgid);
            return 0;
        } else {
            int32_t pgid = *(int32_t *)argp;
            if (pgid <= 0) return -EINVAL;
            ctx->rt->fg_pgid = pgid;
            ydebug("ioctl TIOCSPGRP(virt) <- %d\n", pgid);
            return 0;
        }
    }
    /* TIOCSCTTY: caller wants this tty as its controlling terminal.
     * The kernel's bookkeeping is per-host-process and doesn't fit
     * one-pthread-per-guest-proc; just accept and update the
     * virtualized fg pgrp to the caller's pgrp. */
    if (cmd == 0x540E /*TIOCSCTTY*/ && hfd >= 0 && isatty(hfd)) {
        if (ctx->proc) ctx->rt->fg_pgid = ctx->proc->pgid;
        ydebug("ioctl TIOCSCTTY(virt) fg_pgid <- %d\n", ctx->rt->fg_pgid);
        return 0;
    }

    int r = ioctl(hfd, cmd, argp);
    ydebug("ioctl = %d (errno=%d)\n", r, r < 0 ? errno : 0);
    return r < 0 ? -errno : r;
}

int32_t yos_fcntl(struct yos_exec_ctx *ctx, int32_t fd, int32_t cmd, int32_t arg)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    /* F_DUPFD / F_DUPFD_CLOEXEC return a fresh host fd that needs a
     * wasm-fd slot like dup() does. Other fcntl commands return flags
     * or 0 — pass through unchanged. */
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        int r = fcntl(hfd, cmd, arg);
        if (r < 0) return -errno;
        return yos_fd_alloc(ctx, r);
    }
    int r = fcntl(hfd, cmd, arg);
    return r < 0 ? -errno : r;
}

int32_t yos_vfs_fcntl64(struct yos_exec_ctx *ctx, int32_t fd, int32_t cmd, int32_t arg)
{
    return yos_fcntl(ctx, fd, cmd, arg);
}

int32_t yos_vfs_chroot(struct yos_exec_ctx *ctx, uint32_t filename)
{
    (void)ctx; (void)filename;
    return -EPERM;
}

int32_t yos_symlink(struct yos_exec_ctx *ctx, uint32_t oldpath, uint32_t newpath)
{
    const char *o = wstr(ctx, oldpath);
    const char *n = wstr(ctx, newpath);
    if (!o || !n) return -EFAULT;
    return symlink(o, n) < 0 ? -errno : 0;
}

int32_t yos_readlink(struct yos_exec_ctx *ctx, uint32_t path, uint32_t buf, uint32_t bufsiz)
{
    const char *s = wstr(ctx, path);
    char *b = (char *)wptr(ctx, buf);
    if (!s || !b) return -EFAULT;

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, s, &remaining);
        if (ops && ops->readlink) {
            return ops->readlink(ctx, remaining, b, bufsiz);
        }
    }

    ssize_t r = readlink(s, b, bufsiz);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_truncate(struct yos_exec_ctx *ctx, uint32_t path, int32_t length)
{
    const char *s = wstr(ctx, path);
    if (!s) return -EFAULT;
    return truncate(s, length) < 0 ? -errno : 0;
}

int32_t yos_ftruncate(struct yos_exec_ctx *ctx, int32_t fd, int32_t length)
{
    (void)ctx;
    return ftruncate(host_fd(ctx, fd), length) < 0 ? -errno : 0;
}

int32_t yos_fchmod(struct yos_exec_ctx *ctx, int32_t fd, int32_t mode)
{
    (void)ctx;
    return fchmod(host_fd(ctx, fd), mode) < 0 ? -errno : 0;
}

int32_t yos_fchown(struct yos_exec_ctx *ctx, int32_t fd, int32_t user, int32_t group)
{
    (void)ctx;
    return fchown(host_fd(ctx, fd), user, group) < 0 ? -errno : 0;
}

/* TODO: replace with per-process fd table / custom VFS */
/* Convert a wasm32 iovec[] (8 bytes/entry: u32 base, u32 len) into a host
 * iovec[] (16 bytes/entry: u64 base, u64 len) by translating each base
 * pointer through the wasm linear memory. The host array must already
 * be sized for `vlen` entries. Returns 0 on success, -errno on bad ptr. */
static int yos_iovec_w32_to_host(struct yos_exec_ctx *ctx,
                                  uint32_t wasm_vec, int vlen,
                                  struct iovec *host_iov)
{
    if (vlen <= 0) return 0;
    uint8_t *iov_ptr = wptr(ctx, wasm_vec);
    if (!iov_ptr) return -EFAULT;
    for (int i = 0; i < vlen; i++) {
        uint32_t base = *(uint32_t *)(iov_ptr + i * 8);
        uint32_t len  = *(uint32_t *)(iov_ptr + i * 8 + 4);
        host_iov[i].iov_base = wptr(ctx, base);
        host_iov[i].iov_len = len;
    }
    return 0;
}

int32_t yos_readv(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec, int32_t vlen)
{
    struct iovec host_iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, host_iov);
    if (r) return r;
    ssize_t n = readv(host_fd(ctx, fd), host_iov, vlen);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_writev(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec, int32_t vlen)
{
    struct iovec host_iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, host_iov);
    if (r) return r;
    ssize_t n = writev(host_fd(ctx, fd), host_iov, vlen);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_vfs_pread64(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf, uint32_t count, uint32_t pos)
{
    void *p = wptr(ctx, buf);
    if (!p) return -EFAULT;
    ssize_t r = pread(host_fd(ctx, fd), p, count, pos);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_vfs_pwrite64(struct yos_exec_ctx *ctx, int32_t fd, uint32_t buf, uint32_t count, uint32_t pos)
{
    void *p = wptr(ctx, buf);
    if (!p) return -EFAULT;
    ssize_t r = pwrite(host_fd(ctx, fd), p, count, pos);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_chown(struct yos_exec_ctx *ctx, uint32_t filename, int32_t user, int32_t group)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return chown(s, user, group) < 0 ? -errno : 0;
}

int32_t yos_getcwd(struct yos_exec_ctx *ctx, uint32_t buf, uint32_t size)
{
    char *b = wptr(ctx, buf);
    if (!b) return -EFAULT;
    /* Prefer the per-runtime tracked cwd over host getcwd() so that two
     * forked yos processes (pthreads of one host pid sharing one host
     * cwd) report their own paths after each does its own chdir. Falls
     * back to host getcwd if ctx->cwd hasn't been initialized. */
    if (ctx->cwd[0] == '/') {
        size_t n = strlen(ctx->cwd) + 1;
        if (n > size) return -ERANGE;
        memcpy(b, ctx->cwd, n);
        return (int32_t)buf;
    }
    return getcwd(b, size) ? (int32_t)buf : -errno;
}

int32_t yos_openat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, int32_t flags, int32_t mode)
{
    const char *path = wstr(ctx, filename);
    if (!path) return -EFAULT;

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, path, &remaining);
        if (ops && ops->open) {
            return ops->open(ctx, remaining, flags, mode);
        }
    }

    /* Translate dfd through the per-runtime fd table. AT_FDCWD passes
     * through. Without this, dfd was the wasm-side fd number and the
     * host kernel interpreted it as some unrelated host fd. */
    int host_dfd = (dfd == AT_FDCWD) ? AT_FDCWD : yos_fd_get(ctx, dfd);
    if (host_dfd < 0) return host_dfd;

    int r = openat(host_dfd, path, flags, mode);
    if (r < 0) return -errno;
    /* Allocate a wasm-side fd that maps to the host fd. The previous
     * version returned the raw host fd, which broke the per-runtime fd
     * table — child runtimes that did read(wasm_fd) would translate
     * the same integer to a different host fd via fd_map and hit
     * EBADF. On wasm-fd-table overflow, close the host fd to avoid
     * leaking it. */
    int wfd = yos_fd_alloc(ctx, r);
    if (wfd < 0) close(r);
    return wfd;
}

int32_t yos_mkdirat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t pathname, int32_t mode)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return -EFAULT;
    return mkdirat(dfd, s, mode) < 0 ? -errno : 0;
}

int32_t yos_vfs_mknodat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, int32_t mode, uint32_t dev)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return mknodat(dfd, s, mode, dev) < 0 ? -errno : 0;
}

int32_t yos_fchownat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, int32_t user, int32_t group, int32_t flag)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return fchownat(dfd, s, user, group, flag) < 0 ? -errno : 0;
}

int32_t yos_vfs_fstatat64(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, uint32_t statbuf, int32_t flag)
{
    const char *path = wstr(ctx, filename);
    if (!path) return -EFAULT;

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, path, &remaining);
        if (ops && ops->stat) {
            void *buf = wptr(ctx, statbuf);
            if (!buf) return -EFAULT;
            return ops->stat(ctx, remaining, buf);
        }
    }

    struct stat st;
    int ret = fstatat(dfd, path, &st, flag);
    if (ret < 0) return -errno;
    /* Convert host stat to wasm32 stat64 - simplified, copy key fields */
    if (statbuf && statbuf < ctx->memory_size - 96) {
        uint8_t *buf = ctx->memory + statbuf;
        memset(buf, 0, 96);
        /* stat64 layout for i386 - see include/linux-i386/asm/stat.h */
        *(uint64_t *)(buf + 0) = st.st_dev;
        *(uint32_t *)(buf + 12) = st.st_ino;  /* __st_ino (32-bit) */
        *(uint32_t *)(buf + 16) = st.st_mode;
        *(uint32_t *)(buf + 20) = st.st_nlink;
        *(uint32_t *)(buf + 24) = st.st_uid;
        *(uint32_t *)(buf + 28) = st.st_gid;
        *(uint64_t *)(buf + 32) = st.st_rdev;
        *(int64_t *)(buf + 48) = st.st_size;
        *(uint32_t *)(buf + 56) = st.st_blksize;
        *(uint64_t *)(buf + 64) = st.st_blocks;
        *(uint32_t *)(buf + 72) = st.st_atime;
        *(uint32_t *)(buf + 80) = st.st_mtime;
        *(uint32_t *)(buf + 88) = st.st_ctime;
        *(uint64_t *)(buf + 96 - 8) = st.st_ino; /* st_ino (64-bit) */
    }
    return 0;
}

int32_t yos_unlinkat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t pathname, int32_t flag)
{
    const char *s = wstr(ctx, pathname);
    if (!s) return -EFAULT;
    return unlinkat(dfd, s, flag) < 0 ? -errno : 0;
}

int32_t yos_renameat(struct yos_exec_ctx *ctx, int32_t olddfd, uint32_t oldname, int32_t newdfd, uint32_t newname)
{
    const char *o = wstr(ctx, oldname);
    const char *n = wstr(ctx, newname);
    if (!o || !n) return -EFAULT;
    return renameat(olddfd, o, newdfd, n) < 0 ? -errno : 0;
}

int32_t yos_linkat(struct yos_exec_ctx *ctx, int32_t olddfd, uint32_t oldname, int32_t newdfd, uint32_t newname, int32_t flags)
{
    const char *o = wstr(ctx, oldname);
    const char *n = wstr(ctx, newname);
    if (!o || !n) return -EFAULT;
    return linkat(olddfd, o, newdfd, n, flags) < 0 ? -errno : 0;
}

int32_t yos_symlinkat(struct yos_exec_ctx *ctx, uint32_t oldname, int32_t newdfd, uint32_t newname)
{
    const char *o = wstr(ctx, oldname);
    const char *n = wstr(ctx, newname);
    if (!o || !n) return -EFAULT;
    return symlinkat(o, newdfd, n) < 0 ? -errno : 0;
}

int32_t yos_readlinkat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t path, uint32_t buf, uint32_t bufsiz)
{
    const char *s = wstr(ctx, path);
    char *b = (char *)wptr(ctx, buf);
    if (!s || !b) return -EFAULT;

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, s, &remaining);
        if (ops && ops->readlink) {
            return ops->readlink(ctx, remaining, b, bufsiz);
        }
    }

    ssize_t r = readlinkat(dfd, s, b, bufsiz);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_fchmodat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, int32_t mode)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return fchmodat(dfd, s, mode, 0) < 0 ? -errno : 0;
}

int32_t yos_faccessat(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t filename, int32_t mode)
{
    const char *s = wstr(ctx, filename);
    if (!s) return -EFAULT;
    return faccessat(dfd, s, mode, 0) < 0 ? -errno : 0;
}

/* dup2/dup3: assign a fresh host-fd dup of oldfd into wasm-slot newfd.
 * The dup is mandatory — POSIX dup2(X, Y); close(X) must leave Y open,
 * and with per-runtime fd tables we further need each wasm fd to own
 * its own host fd so close in one runtime doesn't yank the underlying
 * file out from under the other. */
int32_t yos_dup2(struct yos_exec_ctx *ctx, int32_t oldfd, int32_t newfd)
{
    int32_t host_old = yos_fd_get(ctx, oldfd);
    if (host_old < 0) return host_old;
    if (oldfd == newfd) return newfd;  /* POSIX: no-op */
    int host_new = fcntl(host_old, F_DUPFD, 0);
    if (host_new < 0) return -errno;
    return yos_fd_assign(ctx, newfd, host_new);
}

int32_t yos_dup3(struct yos_exec_ctx *ctx, int32_t oldfd, int32_t newfd, int32_t flags)
{
    int32_t host_old = yos_fd_get(ctx, oldfd);
    if (host_old < 0) return host_old;
    if (oldfd == newfd) return -EINVAL;  /* dup3 forbids equal fds */
    int host_new = fcntl(host_old,
                         (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
    if (host_new < 0) return -errno;
    return yos_fd_assign(ctx, newfd, host_new);
}

int32_t yos_pipe2(struct yos_exec_ctx *ctx, uint32_t fildes, int32_t flags)
{
    int *p = wptr(ctx, fildes);
    if (!p) return -EFAULT;
    int hfds[2];
    /* FreeBSD vs Linux flag remap. FreeBSD: O_CLOEXEC=0x00100000,
     * O_NONBLOCK=0x00000004. Linux: O_CLOEXEC=0x00080000,
     * O_NONBLOCK=0x00000800. Translate before calling host pipe2. */
    int hflags = 0;
    if (flags & 0x00100000) hflags |= 0x00080000; /* O_CLOEXEC */
    if (flags & 0x00000004) hflags |= 0x00000800; /* O_NONBLOCK */
    int leftover = flags & ~(0x00100000 | 0x00000004);
    if (leftover) {
        ydebug("yos_pipe2: untranslated flag bits 0x%x (passed through)\n",
               leftover);
        hflags |= leftover;
    }
    if (pipe2(hfds, hflags) < 0) {
        ydebug("yos_pipe2(flags=0x%x->0x%x) host failed: %s\n",
               flags, hflags, strerror(errno));
        return -errno;
    }
    int32_t r = yos_fd_alloc(ctx, hfds[0]);
    if (r < 0) { close(hfds[1]); return r; }
    int32_t w = yos_fd_alloc(ctx, hfds[1]);
    if (w < 0) { yos_fd_close(ctx, r); return w; }
    p[0] = r;
    p[1] = w;
    ydebug("yos_pipe2(flags=0x%x->0x%x) -> wfd[%d, %d]\n",
           flags, hflags, r, w);
    return 0;
}

#include <sys/socket.h>

int32_t yos_vfs_socketpair(struct yos_exec_ctx *ctx, int32_t domain,
                           int32_t type, int32_t protocol, uint32_t sv)
{
    int *p = wptr(ctx, sv);
    if (!p) return -EFAULT;
    int hfds[2];
    if (socketpair(domain, type, protocol, hfds) < 0) return -errno;
    int32_t a = yos_fd_alloc(ctx, hfds[0]);
    if (a < 0) { close(hfds[1]); return a; }
    int32_t b = yos_fd_alloc(ctx, hfds[1]);
    if (b < 0) { yos_fd_close(ctx, a); return b; }
    p[0] = a;
    p[1] = b;
    return 0;
}

int32_t yos_preadv(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec, int32_t vlen, uint32_t pos_l, uint32_t pos_h)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, iov);
    if (r) return r;
    off_t offset = ((off_t)pos_h << 32) | pos_l;
    ssize_t n = preadv(hfd, iov, vlen, offset);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_pwritev(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec, int32_t vlen, uint32_t pos_l, uint32_t pos_h)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, iov);
    if (r) return r;
    off_t offset = ((off_t)pos_h << 32) | pos_l;
    ssize_t n = pwritev(hfd, iov, vlen, offset);
    return n < 0 ? -errno : (int32_t)n;
}

/* preadv2/pwritev2 add a `flags` arg (RWF_HIPRI / RWF_DSYNC / RWF_SYNC etc.)
 * — passed straight through to the kernel. */
int32_t yos_vfs_preadv2(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec,
                        int32_t vlen, uint32_t pos_l, uint32_t pos_h,
                        int32_t flags)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, iov);
    if (r) return r;
    off_t offset = ((off_t)pos_h << 32) | pos_l;
    ssize_t n = preadv2(hfd, iov, vlen, offset, flags);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_vfs_pwritev2(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec,
                         int32_t vlen, uint32_t pos_l, uint32_t pos_h,
                         int32_t flags)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, vlen, iov);
    if (r) return r;
    off_t offset = ((off_t)pos_h << 32) | pos_l;
    ssize_t n = pwritev2(hfd, iov, vlen, offset, flags);
    return n < 0 ? -errno : (int32_t)n;
}

/* vmsplice — pipe fd + iovec[]. Fewer callers than preadv2 but the kernel
 * accepts it through the same iovec ABI. */
int32_t yos_vfs_vmsplice(struct yos_exec_ctx *ctx, int32_t fd, uint32_t vec,
                         uint32_t vlen, uint32_t flags)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, (int)vlen, iov);
    if (r) return r;
    ssize_t n = vmsplice(hfd, iov, vlen, flags);
    return n < 0 ? -errno : (int32_t)n;
}

/* process_madvise — pidfd + iovec[] of memory ranges. Need <sys/mman.h>
 * for MADV_*; the syscall itself is what we wrap. */
int32_t yos_vfs_process_madvise(struct yos_exec_ctx *ctx, int32_t pidfd,
                                uint32_t vec, uint32_t vlen,
                                int32_t behavior, uint32_t flags)
{
    int32_t hfd = yos_fd_get(ctx, pidfd);
    if (hfd < 0) return hfd;
    struct iovec iov[vlen];
    int r = yos_iovec_w32_to_host(ctx, vec, (int)vlen, iov);
    if (r) return r;
    /* Use raw syscall — process_madvise has no glibc wrapper everywhere. */
    long n = syscall(SYS_process_madvise, hfd, iov, (unsigned long)vlen,
                     behavior, flags);
    return n < 0 ? -errno : (int32_t)n;
}

/* process_vm_readv / process_vm_writev — *cross-process* iovec moves.
 * The remote_iov array describes addresses in another process; passing
 * those addresses straight to the kernel is what's intended (it does NOT
 * touch our wasm memory for those). Only the *local* iovec needs
 * translation. */
int32_t yos_vfs_process_vm_readv(struct yos_exec_ctx *ctx, int32_t pid,
                                 uint32_t lvec, uint32_t liovcnt,
                                 uint32_t rvec, uint32_t riovcnt,
                                 uint32_t flags)
{
    struct iovec liov[liovcnt];
    int r = yos_iovec_w32_to_host(ctx, lvec, (int)liovcnt, liov);
    if (r) return r;
    /* Remote iovec: addresses are in the OTHER process — we still have to
     * convert the wasm32 layout to host layout, but the bases stay raw
     * because they're not pointers into our memory. */
    struct iovec riov[riovcnt];
    if (riovcnt) {
        uint8_t *p = wptr(ctx, rvec);
        if (!p) return -EFAULT;
        for (uint32_t i = 0; i < riovcnt; i++) {
            uint32_t base = *(uint32_t *)(p + i * 8);
            uint32_t len  = *(uint32_t *)(p + i * 8 + 4);
            riov[i].iov_base = (void *)(uintptr_t)base;
            riov[i].iov_len = len;
        }
    }
    long n = syscall(SYS_process_vm_readv, pid, liov, (unsigned long)liovcnt,
                     riov, (unsigned long)riovcnt, (unsigned long)flags);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_vfs_process_vm_writev(struct yos_exec_ctx *ctx, int32_t pid,
                                  uint32_t lvec, uint32_t liovcnt,
                                  uint32_t rvec, uint32_t riovcnt,
                                  uint32_t flags)
{
    struct iovec liov[liovcnt];
    int r = yos_iovec_w32_to_host(ctx, lvec, (int)liovcnt, liov);
    if (r) return r;
    struct iovec riov[riovcnt];
    if (riovcnt) {
        uint8_t *p = wptr(ctx, rvec);
        if (!p) return -EFAULT;
        for (uint32_t i = 0; i < riovcnt; i++) {
            uint32_t base = *(uint32_t *)(p + i * 8);
            uint32_t len  = *(uint32_t *)(p + i * 8 + 4);
            riov[i].iov_base = (void *)(uintptr_t)base;
            riov[i].iov_len = len;
        }
    }
    long n = syscall(SYS_process_vm_writev, pid, liov, (unsigned long)liovcnt,
                     riov, (unsigned long)riovcnt, (unsigned long)flags);
    return n < 0 ? -errno : (int32_t)n;
}

int32_t yos_vfs_getdents(struct yos_exec_ctx *ctx, int32_t fd, uint32_t dirent, uint32_t count)
{
    void *p = wptr(ctx, dirent);
    if (!p) return -EFAULT;
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    long r = syscall(SYS_getdents, hfd, p, count);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_vfs_getdents64(struct yos_exec_ctx *ctx, int32_t fd, uint32_t dirent, uint32_t count)
{
    void *p = wptr(ctx, dirent);
    if (!p) return -EFAULT;

    /* Check if virtual fd */
    if (yos_is_virtual_fd(fd)) {
        struct yos_file_table *ft = (struct yos_file_table *)ctx->procfs_fds;
        if (!ft) return -EBADF;
        struct yos_file *file = yos_file_get(ft, fd);
        if (!file) return -EBADF;
        if (file->ops && file->ops->getdents64)
            return file->ops->getdents64(ctx, file, p, count);
        return -EBADF;
    }

    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) return hfd;
    long r = syscall(SYS_getdents64, hfd, p, count);
    return r < 0 ? -errno : (int32_t)r;
}

/*
 * statx - extended stat with mount table support
 * Kernel ABI constants for mode bits
 */
#define YOS_S_IFDIR  0040000
#define YOS_S_IFREG  0100000
#define YOS_S_IFLNK  0120000

int32_t yos_vfs_statx(struct yos_exec_ctx *ctx, int32_t dfd, uint32_t pathname,
                      int32_t flags, uint32_t mask, uint32_t buffer)
{
    const char *path = wstr(ctx, pathname);
    if (!path) return -EFAULT;

    uint8_t *buf = wptr(ctx, buffer);
    if (!buf) return -EFAULT;

    ydebug("yos_vfs_statx: path=%s\n", path);

    /* Check if path is in a virtual filesystem */
    struct yos_mount_table *mt = (struct yos_mount_table *)ctx->rt->mount_table;
    if (mt) {
        const char *remaining;
        const struct yos_file_operations *ops = yos_mount_resolve(mt, path, &remaining);
        if (ops && ops->stat) {
            /*
             * Virtual filesystem - generate statx directly
             * We call the stat op which fills a stat64 buffer, then we
             * need to convert key fields to statx format
             */
            uint8_t stat64_buf[96];
            memset(stat64_buf, 0, sizeof(stat64_buf));
            int32_t ret = ops->stat(ctx, remaining, stat64_buf);
            if (ret < 0) return ret;

            /* Convert stat64 to statx format
             * stat64 layout: mode at +16, nlink at +20
             * statx layout (wasm32_statx):
             *   +0: stx_mask (uint32)
             *   +4: stx_blksize (uint32)
             *   +8: stx_attributes (uint64)
             *   +16: stx_nlink (uint32)
             *   +20: stx_uid (uint32)
             *   +24: stx_gid (uint32)
             *   +28: stx_mode (uint16)
             */
            memset(buf, 0, 256);
            uint32_t mode = *(uint32_t *)(stat64_buf + 16);
            uint32_t nlink = *(uint32_t *)(stat64_buf + 20);

            *(uint32_t *)(buf + 0) = 0x7ff;  /* stx_mask - basic stats */
            *(uint32_t *)(buf + 4) = 4096;   /* stx_blksize */
            *(uint32_t *)(buf + 16) = nlink; /* stx_nlink */
            *(uint16_t *)(buf + 28) = (uint16_t)mode; /* stx_mode */
            *(uint64_t *)(buf + 32) = 1;     /* stx_ino */
            return 0;
        }
    }

    /* Pass through to host statx, translating dfd through host_stdio[]
     * so AT_EMPTY_PATH lookups against wasm fd 0/1/2 hit the right host
     * fd in the parent. AT_FDCWD is special and stays as-is. */
    int host_dfd = (dfd == -100 /* AT_FDCWD */) ? dfd : host_fd(ctx, dfd);
    struct statx host_statx;
    long ret = syscall(__NR_statx, host_dfd, path, flags, mask, &host_statx);
    if (ret < 0) return -errno;

    /* Convert host statx to wasm32 statx - same layout, direct copy */
    memcpy(buf, &host_statx, sizeof(host_statx));
    return 0;
}

/* ============================================================================
 * sendfile / sendfile64 — host's `off_t *offset` is 8 bytes; wasm32's
 * old-style sendfile takes a `long *offset` (4 bytes) and the sendfile64
 * variant takes `loff_t *offset` (already 8). For sendfile (the i386
 * "long *" variant) we read a 32-bit value, run the host syscall with a
 * scratch off_t, and write back with overflow detection.
 * ========================================================================= */

#include <sys/sendfile.h>

int32_t yos_vfs_sendfile(struct yos_exec_ctx *ctx, int32_t out_fd, int32_t in_fd, uint32_t offset_ptr, uint32_t count)
{
    int32_t hout = yos_fd_get(ctx, out_fd);
    if (hout < 0) return hout;
    int32_t hin = yos_fd_get(ctx, in_fd);
    if (hin < 0) return hin;

    off_t off_val = 0;
    off_t *off_arg = NULL;
    int32_t *wasm_off = NULL;
    if (offset_ptr) {
        wasm_off = wptr(ctx, offset_ptr);
        if (!wasm_off) return -EFAULT;
        off_val = (off_t)(int32_t)*wasm_off;  /* sign-extend 4→8 */
        off_arg = &off_val;
    }

    ssize_t r = sendfile(hout, hin, off_arg, count);
    if (r < 0) return -errno;
    if (wasm_off) {
        if (off_val > 0x7fffffffLL || off_val < -0x80000000LL) return -EOVERFLOW;
        *wasm_off = (int32_t)off_val;
    }
    return (int32_t)r;
}

int32_t yos_vfs_sendfile64(struct yos_exec_ctx *ctx, int32_t out_fd, int32_t in_fd, uint32_t offset_ptr, uint32_t count)
{
    /* sendfile64's offset is already loff_t (8 bytes) on both sides;
     * the only thing that differs is the wasm pointer. */
    int32_t hout = yos_fd_get(ctx, out_fd);
    if (hout < 0) return hout;
    int32_t hin = yos_fd_get(ctx, in_fd);
    if (hin < 0) return hin;
    off_t *off_arg = NULL;
    if (offset_ptr) {
        off_arg = (off_t *)wptr(ctx, offset_ptr);
        if (!off_arg) return -EFAULT;
    }
    ssize_t r = sendfile(hout, hin, off_arg, count);
    return r < 0 ? -errno : (int32_t)r;
}

/* ============================================================================
 * POSIX timer_t mapping. The host kernel hands back a `void *` timer_t
 * (8 bytes). wasm32 expects 4 bytes. We keep a per-runtime table that
 * maps wasm-side int32 IDs to host timer_t values; the wasm caller
 * sees a small handle and we look up the real one before each kernel
 * call. Capacity is fixed (rare for one process to have hundreds of
 * timers) — overflow returns EAGAIN.
 * ========================================================================= */

#include <time.h>
#include <signal.h>
#include <pthread.h>

/* Per-ctx timer-id table helpers. The table itself lives in the exec
 * context (ctx->timer_ids[], see types.h); these functions just
 * lock/look-up/free a slot. A global table would let one wasm process
 * delete or read timers another process owns — moving it per-ctx is
 * what the docstring promised but the code didn't implement. */
static void timer_table_init(struct yos_exec_ctx *ctx)
{
    if (ctx->timer_lock_init) return;
    pthread_mutex_init(&ctx->timer_lock, NULL);
    ctx->timer_lock_init = 1;
}

static int timer_table_alloc(struct yos_exec_ctx *ctx, timer_t host_id)
{
    timer_table_init(ctx);
    pthread_mutex_lock(&ctx->timer_lock);
    for (int i = 0; i < YOS_TIMER_MAX; i++) {
        if (!ctx->timer_ids[i]) {
            ctx->timer_ids[i] = host_id;
            pthread_mutex_unlock(&ctx->timer_lock);
            return i + 1;  /* 0 reserved as 'free' */
        }
    }
    pthread_mutex_unlock(&ctx->timer_lock);
    return -1;
}

static timer_t timer_table_get(struct yos_exec_ctx *ctx, int wasm_id)
{
    if (wasm_id < 1 || wasm_id > YOS_TIMER_MAX) return NULL;
    timer_table_init(ctx);
    return ctx->timer_ids[wasm_id - 1];
}

static void timer_table_free(struct yos_exec_ctx *ctx, int wasm_id)
{
    if (wasm_id < 1 || wasm_id > YOS_TIMER_MAX) return;
    timer_table_init(ctx);
    pthread_mutex_lock(&ctx->timer_lock);
    ctx->timer_ids[wasm_id - 1] = NULL;
    pthread_mutex_unlock(&ctx->timer_lock);
}

/* timer_create writes the new ID through `timerid_out`. wasm-side
 * timer_t is 4 bytes; we always store our int32 handle there. The
 * sevp arg (struct sigevent) is converted via the auto-generated
 * sigevent_wasm32_to_host (declared in struct_convert.h). */
int32_t yos_vfs_timer_create(struct yos_exec_ctx *ctx, int32_t clockid, uint32_t sevp, uint32_t timerid_out)
{
    timer_t host_id = NULL;
    int rc;
    if (sevp == 0) {
        rc = timer_create(clockid, NULL, &host_id);
    } else {
        struct host64_sigevent host_sev;
        sigevent_wasm32_to_host(
            (const struct wasm32_sigevent *)wptr(ctx, sevp),
            &host_sev);
        rc = timer_create(clockid, (struct sigevent *)&host_sev, &host_id);
    }
    if (rc < 0) return -errno;
    int wid = timer_table_alloc(ctx, host_id);
    if (wid < 0) { timer_delete(host_id); return -EAGAIN; }
    int32_t *out = wptr(ctx, timerid_out);
    if (!out) { timer_delete(host_id); timer_table_free(ctx, wid); return -EFAULT; }
    *out = wid;
    return 0;
}

int32_t yos_vfs_timer_settime(struct yos_exec_ctx *ctx, int32_t timerid, int32_t flags, uint32_t new_value, uint32_t old_value)
{
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    struct host64___kernel_itimerspec host_new, host_old;
    if (!new_value) return -EFAULT;
    void *wp = wptr(ctx, new_value);
    if (!wp) return -EFAULT;
    __kernel_itimerspec_wasm32_to_host(
        (const struct wasm32___kernel_itimerspec *)wp, &host_new);
    int rc = timer_settime(hid, flags, (struct itimerspec *)&host_new,
                            old_value ? (struct itimerspec *)&host_old : NULL);
    if (rc < 0) return -errno;
    if (old_value) {
        void *wo = wptr(ctx, old_value);
        if (wo) __kernel_itimerspec_host_to_wasm32(
            &host_old, (struct wasm32___kernel_itimerspec *)wo);
    }
    return 0;
}

int32_t yos_vfs_timer_settime64(struct yos_exec_ctx *ctx, int32_t timerid, int32_t flags, uint32_t new_value, uint32_t old_value)
{
    /* _time64 variant: wasm passes the modern 16-byte timespec; layouts
     * match host's __kernel_itimerspec. Plain memcpy + passthrough. */
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    if (!new_value) return -EFAULT;
    struct itimerspec *host_new = wptr(ctx, new_value);
    struct itimerspec *host_old = old_value ? wptr(ctx, old_value) : NULL;
    if (!host_new) return -EFAULT;
    int rc = timer_settime(hid, flags, host_new, host_old);
    return rc < 0 ? -errno : 0;
}

int32_t yos_vfs_timer_gettime(struct yos_exec_ctx *ctx, int32_t timerid, uint32_t cur_value)
{
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    struct host64___kernel_itimerspec host_val;
    int rc = timer_gettime(hid, (struct itimerspec *)&host_val);
    if (rc < 0) return -errno;
    void *wo = wptr(ctx, cur_value);
    if (!wo) return -EFAULT;
    __kernel_itimerspec_host_to_wasm32(
        &host_val, (struct wasm32___kernel_itimerspec *)wo);
    return 0;
}

int32_t yos_vfs_timer_gettime64(struct yos_exec_ctx *ctx, int32_t timerid, uint32_t cur_value)
{
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    struct itimerspec *host_val = wptr(ctx, cur_value);
    if (!host_val) return -EFAULT;
    int rc = timer_gettime(hid, host_val);
    return rc < 0 ? -errno : 0;
}

int32_t yos_vfs_timer_delete(struct yos_exec_ctx *ctx, int32_t timerid)
{
    (void)ctx;
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    int rc = timer_delete(hid);
    timer_table_free(ctx, timerid);
    return rc < 0 ? -errno : 0;
}

int32_t yos_vfs_timer_getoverrun(struct yos_exec_ctx *ctx, int32_t timerid)
{
    (void)ctx;
    timer_t hid = timer_table_get(ctx, timerid);
    if (!hid) return -EINVAL;
    int rc = timer_getoverrun(hid);
    return rc < 0 ? -errno : rc;
}

/* ============================================================================
 * futex with optional old_timespec32 timeout. The host's futex syscall
 * (#202 on x86_64) takes `struct __kernel_timespec *` (16 bytes); wasm
 * musl passes `struct old_timespec32 *` (8 bytes). NULL is valid and
 * means "no timeout". val2 is also reused for FUTEX_WAKE_OP etc.
 * ========================================================================= */

/* ============================================================================
 * NUMA mempolicy: bitmask arrays of `unsigned long`. wasm32 elements
 * are 4 bytes, host64 are 8. The kernel reads ceil(maxnode/8) bytes
 * regardless. We widen by zero-padding the high 32 bits of each
 * 64-bit slot (and narrow by checking the high bits are zero — if a
 * NUMA node ID > 32 came back from the kernel, the wasm side can't
 * represent it and we return -EOVERFLOW).
 * ========================================================================= */

static int nmask_w32_to_host(struct yos_exec_ctx *ctx, uint32_t wasm_addr,
                              uint32_t maxnode, uint64_t *host_buf,
                              size_t host_cap)
{
    if (!wasm_addr) return 0;
    size_t bits  = maxnode;
    size_t words = (bits + 63) / 64;  /* host 64-bit words */
    if (words > host_cap) return -EINVAL;
    /* wasm side: ceil(bits/32) 32-bit words. */
    size_t w32_words = (bits + 31) / 32;
    const uint32_t *wp = wptr(ctx, wasm_addr);
    if (!wp) return -EFAULT;
    for (size_t i = 0; i < words; i++) {
        uint32_t lo = (i*2     < w32_words) ? wp[i*2]     : 0;
        uint32_t hi = (i*2 + 1 < w32_words) ? wp[i*2 + 1] : 0;
        host_buf[i] = (uint64_t)lo | ((uint64_t)hi << 32);
    }
    return 0;
}

static int nmask_host_to_w32(struct yos_exec_ctx *ctx, uint32_t wasm_addr,
                              uint32_t maxnode, const uint64_t *host_buf)
{
    if (!wasm_addr) return 0;
    size_t bits  = maxnode;
    size_t words = (bits + 63) / 64;
    size_t w32_words = (bits + 31) / 32;
    uint32_t *wp = wptr(ctx, wasm_addr);
    if (!wp) return -EFAULT;
    for (size_t i = 0; i < words; i++) {
        uint64_t v = host_buf[i];
        if (i*2 < w32_words) wp[i*2]     = (uint32_t)(v & 0xffffffffu);
        if (i*2 + 1 < w32_words) wp[i*2 + 1] = (uint32_t)(v >> 32);
    }
    return 0;
}

#define NMASK_HOST_MAX 16   /* covers up to 1024 NUMA nodes */

int32_t yos_vfs_set_mempolicy(struct yos_exec_ctx *ctx, int32_t mode, uint32_t nmask, uint32_t maxnode)
{
    uint64_t host[NMASK_HOST_MAX] = {0};
    int r = nmask_w32_to_host(ctx, nmask, maxnode, host, NMASK_HOST_MAX);
    if (r < 0) return r;
    long ret = syscall(SYS_set_mempolicy, mode,
                        nmask ? (long)host : 0L, (long)maxnode);
    return ret < 0 ? -errno : (int32_t)ret;
}

int32_t yos_vfs_get_mempolicy(struct yos_exec_ctx *ctx, uint32_t mode, uint32_t nmask, uint32_t maxnode, uint32_t addr, uint32_t flags)
{
    uint64_t host[NMASK_HOST_MAX] = {0};
    int *mode_p = mode ? wptr(ctx, mode) : NULL;
    long ret = syscall(SYS_get_mempolicy, (long)mode_p,
                        nmask ? (long)host : 0L,
                        (long)maxnode, (long)addr, (long)flags);
    if (ret < 0) return -errno;
    if (nmask) nmask_host_to_w32(ctx, nmask, maxnode, host);
    return (int32_t)ret;
}

int32_t yos_vfs_mbind(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len, int32_t mode, uint32_t nmask, uint32_t maxnode, uint32_t flags)
{
    uint64_t host[NMASK_HOST_MAX] = {0};
    int r = nmask_w32_to_host(ctx, nmask, maxnode, host, NMASK_HOST_MAX);
    if (r < 0) return r;
    long ret = syscall(SYS_mbind, (long)(uintptr_t)(ctx->memory + start),
                        (long)len, (long)mode,
                        nmask ? (long)host : 0L, (long)maxnode, (long)flags);
    return ret < 0 ? -errno : (int32_t)ret;
}

int32_t yos_vfs_migrate_pages(struct yos_exec_ctx *ctx, int32_t pid, uint32_t maxnode, uint32_t old_nodes, uint32_t new_nodes)
{
    uint64_t old_host[NMASK_HOST_MAX] = {0};
    uint64_t new_host[NMASK_HOST_MAX] = {0};
    int r;
    if ((r = nmask_w32_to_host(ctx, old_nodes, maxnode, old_host, NMASK_HOST_MAX)) < 0) return r;
    if ((r = nmask_w32_to_host(ctx, new_nodes, maxnode, new_host, NMASK_HOST_MAX)) < 0) return r;
    long ret = syscall(SYS_migrate_pages, (long)pid, (long)maxnode,
                        old_nodes ? (long)old_host : 0L,
                        new_nodes ? (long)new_host : 0L);
    return ret < 0 ? -errno : (int32_t)ret;
}

/* ============================================================================
 * execveat: like execve but with dirfd + flags. The argv/envp arrays
 * are pointer-to-pointer — same shape as execve already handles, so
 * we delegate by setting up an exec_pending and reusing the outer
 * loop. For brevity we just call back into yos_execve via path
 * resolved through dirfd; supports AT_EMPTY_PATH (re-exec self).
 * ========================================================================= */

extern int32_t yos_execve(struct yos_exec_ctx *ctx, uint32_t filename,
                                uint32_t argv, uint32_t envp);

int32_t yos_vfs_execveat(struct yos_exec_ctx *ctx, int32_t dirfd, uint32_t pathname, uint32_t argv, uint32_t envp, int32_t flags)
{
    /* Resolve dirfd-relative pathname to an absolute path so we can
     * reuse yos_execve. AT_EMPTY_PATH means "use whatever dirfd
     * points to" (Linux 3.18+). */
    const char *p = wstr(ctx, pathname);
    if (!p) return -EFAULT;
    char resolved[PATH_MAX];
    if (dirfd == -100 /* AT_FDCWD */ || p[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", p);
    } else if ((flags & 0x1000 /* AT_EMPTY_PATH */) && p[0] == '\0') {
        /* /proc/self/fd/<dirfd> resolves to the underlying executable. */
        int hfd = yos_fd_get(ctx, dirfd);
        if (hfd < 0) return hfd;
        snprintf(resolved, sizeof(resolved), "/proc/self/fd/%d", hfd);
    } else {
        int hfd = yos_fd_get(ctx, dirfd);
        if (hfd < 0) return hfd;
        snprintf(resolved, sizeof(resolved), "/proc/self/fd/%d/%s", hfd, p);
    }
    /* Marshal a fresh wasm-side filename string into a scratch slot.
     * Easier: just copy into a temp wasm buffer at heap_end and call
     * yos_execve with that wasm address. */
    size_t need = strlen(resolved) + 1;
    if (ctx->heap_end + need > ctx->memory_size) return -ENOMEM;
    uint32_t scratch = ctx->heap_end;
    memcpy(ctx->memory + scratch, resolved, need);
    return yos_execve(ctx, scratch, argv, envp);
}

/* ============================================================================
 * get_robust_list: head_ptr stores the wasm-side robust_list_head pointer.
 * The kernel doesn't dereference it; it just records what was given via
 * set_robust_list. Just translate the outer level.
 * ========================================================================= */

int32_t yos_vfs_get_robust_list(struct yos_exec_ctx *ctx, int32_t pid, uint32_t head_ptr, uint32_t len_ptr)
{
    /* The kernel returns pid 0's saved head — likely a wasm address or
     * NULL. Pass the outer pointers through; the inner address is
     * opaque. */
    void *hp = head_ptr ? wptr(ctx, head_ptr) : NULL;
    void *lp = len_ptr ? wptr(ctx, len_ptr) : NULL;
    long r = syscall(SYS_get_robust_list, (long)pid, (long)hp, (long)lp);
    return r < 0 ? -errno : (int32_t)r;
}

/* ============================================================================
 * Linux AIO. aio_context_t is unsigned long — 4 bytes on wasm32,
 * 8 bytes on the host. Real handles fit in 32 bits in practice
 * (kernel returns small refcount IDs), so we narrow with overflow
 * detection. iocb** in io_submit needs per-pointer translation.
 * ========================================================================= */

int32_t yos_vfs_io_setup(struct yos_exec_ctx *ctx, uint32_t nr_events, uint32_t ctx_idp)
{
    if (!ctx_idp) return -EFAULT;
    unsigned long host_id = 0;
    long r = syscall(SYS_io_setup, (unsigned long)nr_events, &host_id);
    if (r < 0) return -errno;
    if (host_id > 0xffffffffUL) {
        long _ignore = syscall(SYS_io_destroy, host_id); (void)_ignore;
        return -EOVERFLOW;
    }
    uint32_t *out = wptr(ctx, ctx_idp);
    if (!out) { long _i = syscall(SYS_io_destroy, host_id); (void)_i; return -EFAULT; }
    *out = (uint32_t)host_id;
    return 0;
}

int32_t yos_vfs_io_destroy(struct yos_exec_ctx *ctx, uint32_t ctx_id)
{
    (void)ctx;
    long r = syscall(SYS_io_destroy, (unsigned long)ctx_id);
    return r < 0 ? -errno : 0;
}

int32_t yos_vfs_io_submit(struct yos_exec_ctx *ctx, uint32_t ctx_id, int32_t nr, uint32_t iocbpp)
{
    if (nr <= 0) return 0;
    uint32_t *wpp = wptr(ctx, iocbpp);
    if (!wpp) return -EFAULT;
    /* Build a host-side array of iocb pointers. The IOCBs themselves
     * stay in wasm memory; the kernel reads through these pointers,
     * but it expects the iocb layout to be the host's 64-bit one. We
     * convert each iocb in place into a per-call scratch buffer and
     * pass scratch addresses. Cap at 64 IOCBs per submit; bigger
     * batches are rare and can be split by the caller. */
    if (nr > 64) return -E2BIG;
    struct host64_iocb scratch[64];
    struct iocb *hpp[64];
    for (int i = 0; i < nr; i++) {
        uint32_t w_iocb = wpp[i];
        if (!w_iocb) return -EFAULT;
        const struct wasm32_iocb *wcb = wptr(ctx, w_iocb);
        if (!wcb) return -EFAULT;
        iocb_wasm32_to_host(wcb, &scratch[i]);
        hpp[i] = (struct iocb *)&scratch[i];
    }
    long r = syscall(SYS_io_submit, (unsigned long)ctx_id, (long)nr, (long)hpp);
    return r < 0 ? -errno : (int32_t)r;
}

int32_t yos_vfs_io_cancel(struct yos_exec_ctx *ctx, uint32_t ctx_id, uint32_t iocb_addr, uint32_t result)
{
    const struct wasm32_iocb *wcb = wptr(ctx, iocb_addr);
    if (!wcb) return -EFAULT;
    struct host64_iocb host_cb;
    iocb_wasm32_to_host(wcb, &host_cb);
    struct host64_io_event host_ev;
    long r = syscall(SYS_io_cancel, (unsigned long)ctx_id,
                      (long)&host_cb, (long)&host_ev);
    if (r < 0) return -errno;
    if (result) {
        struct wasm32_io_event *wev = wptr(ctx, result);
        if (wev) io_event_host_to_wasm32(&host_ev, wev);
    }
    return 0;
}

int32_t yos_vfs_futex(struct yos_exec_ctx *ctx, uint32_t uaddr, int32_t op, int32_t val, uint32_t utime, uint32_t uaddr2, int32_t val3)
{
    /* Use the host's struct timespec from <time.h>; on x86_64 the kernel
     * accepts that shape directly and the field offsets match
     * __kernel_timespec (both int64 sec / int64 nsec). */
    struct timespec ts_buf;
    struct timespec *ts_arg = NULL;
    /* Some FUTEX_* ops reinterpret utime as a small unsigned int (the
     * 'val2' in FUTEX_REQUEUE et al.). The kernel only treats it as a
     * timespec pointer for FUTEX_WAIT / FUTEX_LOCK_PI / etc. */
    int futex_op = op & 0x7f;  /* strip FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME */
    int has_timeout = (futex_op == 0  /* FUTEX_WAIT */ ||
                        futex_op == 6  /* FUTEX_WAIT_BITSET */ ||
                        futex_op == 8  /* FUTEX_LOCK_PI */ ||
                        futex_op == 11 /* FUTEX_WAIT_REQUEUE_PI */ );
    if (utime && has_timeout) {
        const int32_t *wts = wptr(ctx, utime);
        if (!wts) return -EFAULT;
        ts_buf.tv_sec  = (int64_t)(int32_t)wts[0];
        ts_buf.tv_nsec = (int64_t)(int32_t)wts[1];
        ts_arg = &ts_buf;
    }
    void *uaddr_p  = wptr(ctx, uaddr);
    void *uaddr2_p = uaddr2 ? wptr(ctx, uaddr2) : NULL;
    /* Use raw syscall — futex's per-op semantics defy the glibc wrapper. */
    long r = syscall(SYS_futex, uaddr_p, op, (long)val,
                     has_timeout ? (long)ts_arg : (long)utime,
                     uaddr2_p, (long)val3);
    return r < 0 ? -errno : (int32_t)r;
}
