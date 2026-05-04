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
    if (ydebug_enabled()) {
        pid_t tid = (pid_t)syscall(SYS_gettid);
        ydebug("read(tid=%d wfd=%d hfd=%d count=%u) = %zd%s%.*s%s\n",
               (int)tid, fd, hfd, count, r,
               r > 0 ? " head=\"" : "",
               (int)(r > 0 ? (r > 16 ? 16 : r) : 0), (const char *)p,
               r > 0 ? "\"" : "");
    }
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
    if (ydebug_enabled() && fd != 4 && fd != 5) {
        pid_t tid = (pid_t)syscall(SYS_gettid);
        ydebug("write(tid=%d wfd=%d hfd=%d count=%u) = %zd%s%.*s%s\n",
               (int)tid, fd, hfd, count, r,
               r > 0 ? " head=\"" : "",
               (int)(r > 0 ? (r > 32 ? 32 : r) : 0), (const char *)p,
               r > 0 ? "\"" : "");
    }
    return r < 0 ? -errno : (int32_t)r;
}

/* Forward decls — definitions are further down with the fcntl
 * cmd/oflags translation tables. */
static int oflags_fb_to_lx(int f);
static int oflags_lx_to_fb(int f);

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

    int hflags = oflags_fb_to_lx(flags);
    /* open() is `int open(const char *path, int flags, ...)` in
     * FreeBSD headers — variadic. clang's wasm32 ABI passes the
     * variadic mode arg via a va_list pointer in the shadow stack,
     * NOT as a direct i32. The `mode` parameter we receive is a wasm
     * offset to a small struct containing the mode int. Pulling
     * the literal `mode` value used to set garbage permission bits
     * (the wasm stack address looked like mode_t≈0x100000), which
     * surfaced as e.g. shada files created `--w-rw---T` and then
     * unreadable on the next nvim run ("permission denied"). */
    int real_mode = mode;
    if (hflags & O_CREAT) {
        if (mode && (uint32_t)mode + 4 <= ctx->memory_size)
            real_mode = *(int32_t *)(ctx->memory + (uint32_t)mode);
    } else {
        /* Without O_CREAT mode is ignored; don't deref a stack address
         * that may be 0 / past memory. */
        real_mode = 0;
    }
    int r = open(s, hflags, real_mode);
    if (ydebug_enabled())
        ydebug("open(\"%s\" flags=0x%x->0x%x mode_off=%d real_mode=0%o) = %d%s\n",
               s, flags, hflags, mode, real_mode, r,
               r < 0 ? strerror(errno) : "");
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

/* FreeBSD ioctl request numbers (from sys/ttycom.h, sys/filio.h) that
 * nvim/libuv reach for. Linux's request numbers for the equivalent
 * operations are completely different — same operation, different
 * encoding scheme — so the wasm guest's `ioctl(tty_fd, FB_TIOCGWINSZ)`
 * passed straight to host glibc returns ENOTTY because Linux doesn't
 * recognise the request. Translate request → request, then call host
 * ioctl. The arg buffer (struct winsize / int) is layout-compatible
 * for the ones we need so no struct conversion is required. */
#define FB_TIOCGWINSZ   0x40087468u   /* _IOR('t', 0x68, struct winsize) */
#define FB_TIOCSWINSZ   0x80087467u
#define FB_TIOCGPGRP    0x40047477u   /* _IOR('t', 0x77, int) */
#define FB_TIOCSPGRP    0x80047476u
#define FB_TIOCSCTTY    0x20007461u
#define FB_TIOCNOTTY    0x20007471u
#define FB_FIONREAD     0x4004667fu
#define FB_FIONBIO      0x8004667eu
#define FB_FIOCLEX      0x20006601u
#define FB_FIONCLEX     0x20006602u
#define FB_FIOASYNC     0x8004667du

#define LX_TIOCGWINSZ   0x5413u
#define LX_TIOCSWINSZ   0x5414u
#define LX_TIOCGPGRP    0x540Fu
#define LX_TIOCSPGRP    0x5410u
#define LX_TIOCSCTTY    0x540Eu
#define LX_TIOCNOTTY    0x5422u
#define LX_FIONREAD     0x541Bu
#define LX_FIONBIO      0x5421u
#define LX_FIOCLEX      0x5451u
#define LX_FIONCLEX     0x5450u
#define LX_FIOASYNC     0x5452u

static uint32_t ioctl_cmd_fb_to_lx(uint32_t cmd)
{
    switch (cmd) {
    case FB_TIOCGWINSZ: return LX_TIOCGWINSZ;
    case FB_TIOCSWINSZ: return LX_TIOCSWINSZ;
    case FB_TIOCGPGRP:  return LX_TIOCGPGRP;
    case FB_TIOCSPGRP:  return LX_TIOCSPGRP;
    case FB_TIOCSCTTY:  return LX_TIOCSCTTY;
    case FB_TIOCNOTTY:  return LX_TIOCNOTTY;
    case FB_FIONREAD:   return LX_FIONREAD;
    case FB_FIONBIO:    return LX_FIONBIO;
    case FB_FIOCLEX:    return LX_FIOCLEX;
    case FB_FIONCLEX:   return LX_FIONCLEX;
    case FB_FIOASYNC:   return LX_FIOASYNC;
    default:            return cmd;  /* pass through, may still fail */
    }
}

int32_t yos_ioctl(struct yos_exec_ctx *ctx, int32_t fd, uint32_t cmd, uint32_t arg)
{
    uint32_t lcmd = ioctl_cmd_fb_to_lx(cmd);

    /* ioctl is variadic in C; clang's wasm32 ABI passes the third
     * argument via a va_list pointer on the shadow stack — `arg` is
     * the offset of that pack, NOT the offset of the user buffer. The
     * first slot of the pack is the user's actual wasm pointer (or
     * the integer value, for value-arg ioctls). Without this
     * dereference, the host kernel writes/reads the pack location
     * instead of the user's struct, and TIOCGWINSZ silently leaves
     * the caller's `struct winsize` zero — nvim's tui_guess_size
     * then falls through to terminfo defaults (80×24).
     *
     * For value-arg ioctls (TIOCSCTTY, FIONBIO with int*, ...) the
     * dereferenced word is either the int value (passed directly)
     * or another wasm pointer; treating it as a pointer and passing
     * the host translation works for both cases — the host kernel
     * interprets the third arg per the request encoding. */
    uint32_t user_arg = arg;
    if (arg && (uint32_t)arg + 4 <= ctx->memory_size) {
        user_arg = *(uint32_t *)(ctx->memory + arg);
    }

    ydebug("ioctl(tid=%d fd=%d, cmd=0x%x->0x%x, va_pack=0x%x user_arg=0x%x)\n",
           (int)syscall(SYS_gettid), fd, cmd, lcmd, arg, user_arg);

    int hfd = host_fd(ctx, fd);
    void *argp = user_arg ? wptr(ctx, user_arg) : NULL;

    /* Virtualize controlling-tty foreground pgrp queries/sets so the
     * pgid the wasm caller stores/reads belongs to the *guest* pid
     * namespace (matching getpgrp(), setpgid()) rather than the host
     * shell's pgrp the kernel would report. Only intercept on tty fds
     * — non-tty TIOCGPGRP/TIOCSPGRP would just fail with ENOTTY which
     * is the correct kernel behavior. */
    if ((lcmd == LX_TIOCGPGRP || lcmd == LX_TIOCSPGRP)
        && hfd >= 0 && isatty(hfd)) {
        if (!argp) return -EFAULT;
        if (lcmd == LX_TIOCGPGRP) {
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
    if (lcmd == LX_TIOCSCTTY && hfd >= 0 && isatty(hfd)) {
        if (ctx->proc) ctx->rt->fg_pgid = ctx->proc->pgid;
        ydebug("ioctl TIOCSCTTY(virt) fg_pgid <- %d\n", ctx->rt->fg_pgid);
        return 0;
    }

    int r = ioctl(hfd, lcmd, argp);
    ydebug("ioctl = %d (errno=%d)\n", r, r < 0 ? errno : 0);
    if (lcmd == LX_TIOCGWINSZ && r == 0 && argp) {
        unsigned short *ws = (unsigned short *)argp;
        ydebug("  winsize: row=%u col=%u xpix=%u ypix=%u\n",
               ws[0], ws[1], ws[2], ws[3]);
    }
    return r < 0 ? -errno : r;
}

/* fcntl command numbers diverge between FreeBSD and Linux past the
 * common 0..4 range. Most importantly nvim's `fcntl(fd, F_DUPFD_CLOEXEC,
 * 3)` (in channel_from_stdio for the embedded server) passes the
 * FreeBSD value (17); host Linux glibc expects 1030. Without this
 * translation the call returns -1, nvim asserts in stream_init, and
 * the embedded server crashes — leaving the TUI parent's RPC writes
 * to EPIPE. Add the translation table and the matching arg/flag
 * remap for F_GETFL/F_SETFL (O_* values also differ). */
#define FB_F_DUPFD              0
#define FB_F_GETFD              1
#define FB_F_SETFD              2
#define FB_F_GETFL              3
#define FB_F_SETFL              4
#define FB_F_GETOWN             5
#define FB_F_SETOWN             6
#define FB_F_GETLK              11
#define FB_F_SETLK              12
#define FB_F_SETLKW             13
#define FB_F_DUPFD_CLOEXEC      17
#define FB_F_DUP2FD_CLOEXEC     18

static int fcntl_cmd_fb_to_lx(int cmd)
{
    switch (cmd) {
    case FB_F_DUPFD:           return F_DUPFD;
    case FB_F_GETFD:           return F_GETFD;
    case FB_F_SETFD:           return F_SETFD;
    case FB_F_GETFL:           return F_GETFL;
    case FB_F_SETFL:           return F_SETFL;
    case FB_F_GETOWN:          return F_GETOWN;
    case FB_F_SETOWN:          return F_SETOWN;
    case FB_F_GETLK:           return F_GETLK;
    case FB_F_SETLK:           return F_SETLK;
    case FB_F_SETLKW:          return F_SETLKW;
    case FB_F_DUPFD_CLOEXEC:   return F_DUPFD_CLOEXEC;
    default:                   return cmd;  /* pass through, may EINVAL */
    }
}

/* O_* flag bits — FreeBSD vs Linux. Used by open/openat/fcntl(F_*FL).
 * Bottom 2 bits (RDONLY/WRONLY/RDWR) match. The rest is per-flag
 * remap: most differ in BIT POSITION. Without translation, opens with
 * `O_CREAT | O_EXCL` (FreeBSD: 0x200|0x800) get sent to host glibc as
 * O_NOCTTY|O_NDELAY which neither creates nor enforces exclusivity —
 * nvim's swap-file mkstemp loop fails through every variant name
 * and surfaces as E326/E303. */
#define FB_O_NONBLOCK   0x00000004
#define FB_O_APPEND     0x00000008
#define FB_O_SHLOCK     0x00000010
#define FB_O_EXLOCK     0x00000020
#define FB_O_ASYNC      0x00000040
#define FB_O_SYNC       0x00000080
#define FB_O_NOFOLLOW   0x00000100
#define FB_O_CREAT      0x00000200
#define FB_O_TRUNC      0x00000400
#define FB_O_EXCL       0x00000800
#define FB_O_NOCTTY     0x00008000
#define FB_O_DIRECT     0x00010000
#define FB_O_DIRECTORY  0x00020000
#define FB_O_EXEC       0x00040000
#define FB_O_TTY_INIT   0x00080000
#define FB_O_CLOEXEC    0x00100000
#define FB_O_PATH       0x00400000

#define LX_O_NONBLOCK   0x00000800
#define LX_O_APPEND     0x00000400
#define LX_O_ASYNC      0x00002000
#define LX_O_SYNC       0x00101000
#define LX_O_NOFOLLOW   0x00020000
#define LX_O_CREAT      0x00000040
#define LX_O_TRUNC      0x00000200
#define LX_O_EXCL       0x00000080
#define LX_O_NOCTTY     0x00000100
#define LX_O_DIRECT     0x00004000
#define LX_O_DIRECTORY  0x00010000
#define LX_O_PATH       0x00200000
#define LX_O_CLOEXEC    0x00080000

int oflags_fb_to_lx_fwd(int);  /* exported for impl/posix.c */
static int oflags_fb_to_lx(int f)
{
    int r = (f & 3);
    if (f & FB_O_NONBLOCK)   r |= LX_O_NONBLOCK;
    if (f & FB_O_APPEND)     r |= LX_O_APPEND;
    if (f & FB_O_ASYNC)      r |= LX_O_ASYNC;
    if (f & FB_O_SYNC)       r |= LX_O_SYNC;
    if (f & FB_O_NOFOLLOW)   r |= LX_O_NOFOLLOW;
    if (f & FB_O_CREAT)      r |= LX_O_CREAT;
    if (f & FB_O_TRUNC)      r |= LX_O_TRUNC;
    if (f & FB_O_EXCL)       r |= LX_O_EXCL;
    if (f & FB_O_NOCTTY)     r |= LX_O_NOCTTY;
    if (f & FB_O_DIRECT)     r |= LX_O_DIRECT;
    if (f & FB_O_DIRECTORY)  r |= LX_O_DIRECTORY;
    if (f & FB_O_EXEC)       r |= LX_O_PATH;     /* closest match */
    if (f & FB_O_CLOEXEC)    r |= LX_O_CLOEXEC;
    if (f & FB_O_PATH)       r |= LX_O_PATH;
    /* SHLOCK/EXLOCK/TTY_INIT have no Linux equivalent — drop. */
    return r;
}

int oflags_fb_to_lx_fwd(int f) { return oflags_fb_to_lx(f); }

static int oflags_lx_to_fb(int f)
{
    int r = (f & 3);
    if (f & LX_O_NONBLOCK)   r |= FB_O_NONBLOCK;
    if (f & LX_O_APPEND)     r |= FB_O_APPEND;
    if (f & LX_O_ASYNC)      r |= FB_O_ASYNC;
    if (f & LX_O_SYNC)       r |= FB_O_SYNC;
    if (f & LX_O_NOFOLLOW)   r |= FB_O_NOFOLLOW;
    if (f & LX_O_CREAT)      r |= FB_O_CREAT;
    if (f & LX_O_TRUNC)      r |= FB_O_TRUNC;
    if (f & LX_O_EXCL)       r |= FB_O_EXCL;
    if (f & LX_O_NOCTTY)     r |= FB_O_NOCTTY;
    if (f & LX_O_DIRECT)     r |= FB_O_DIRECT;
    if (f & LX_O_DIRECTORY)  r |= FB_O_DIRECTORY;
    if (f & LX_O_CLOEXEC)    r |= FB_O_CLOEXEC;
    if (f & LX_O_PATH)       r |= FB_O_PATH;
    return r;
}

int32_t yos_fcntl(struct yos_exec_ctx *ctx, int32_t fd, int32_t cmd, int32_t arg)
{
    int32_t hfd = yos_fd_get(ctx, fd);
    if (hfd < 0) {
        ydebug("fcntl(wfd=%d) -> EBADF (no fd_map entry)\n", fd);
        return -EBADF;
    }
    int hcmd = fcntl_cmd_fb_to_lx(cmd);
    /* fcntl is declared `int fcntl(int fd, int cmd, ...)` in the
     * FreeBSD headers nvim was built against. clang's wasm32 ABI
     * passes the variadic arg via a va_list pointer in the shadow
     * stack, NOT as a direct i32 — so the `arg` parameter we
     * receive is a wasm offset into a small struct of varargs.
     * Read the actual int from the first slot. (Variadic ints are
     * 4-byte-aligned in clang's wasm32 va layout — see
     * impl/printf.c's va_align for the same convention.) */
    int real_arg = arg;
    if (hcmd == F_DUPFD || hcmd == F_DUPFD_CLOEXEC ||
        hcmd == F_SETFD || hcmd == F_SETFL ||
        hcmd == F_SETOWN) {
        if (arg && (uint32_t)arg + 4 <= ctx->memory_size)
            real_arg = *(int32_t *)(ctx->memory + (uint32_t)arg);
    }
    if (ydebug_enabled())
        ydebug("fcntl(wfd=%d hfd=%d cmd=%d->%d va_off=%d arg=%d)\n",
               fd, hfd, cmd, hcmd, arg, real_arg);
    /* F_DUPFD / F_DUPFD_CLOEXEC return a fresh host fd that needs a
     * wasm-fd slot like dup() does. Other fcntl commands return flags
     * or 0 — pass through unchanged. */
    if (hcmd == F_DUPFD || hcmd == F_DUPFD_CLOEXEC) {
        int r = fcntl(hfd, hcmd, real_arg);
        if (r < 0) return -errno;
        return yos_fd_alloc(ctx, r);
    }
    if (hcmd == F_SETFL) {
        int r = fcntl(hfd, hcmd, oflags_fb_to_lx(real_arg));
        return r < 0 ? -errno : r;
    }
    if (hcmd == F_GETFL) {
        int r = fcntl(hfd, hcmd, 0);
        if (r < 0) return -errno;
        return oflags_lx_to_fb(r);
    }
    int r = fcntl(hfd, hcmd, real_arg);
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
    int hfd = host_fd(ctx, fd);
    ssize_t n = writev(hfd, host_iov, vlen);
    if (ydebug_enabled()) {
        size_t total = 0;
        for (int i = 0; i < vlen; i++) total += host_iov[i].iov_len;
        pid_t tid = (pid_t)syscall(SYS_gettid);
        ydebug("writev(tid=%d wfd=%d hfd=%d vlen=%d total=%zu) = %zd%s\n",
               (int)tid, fd, hfd, vlen, total, n,
               n < 0 ? strerror(errno) : "");
    }
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

    int hflags = oflags_fb_to_lx(flags);
    /* See yos_open: openat is also variadic; mode comes via a wasm
     * va_list pointer when O_CREAT is set. */
    int real_mode = mode;
    if (hflags & O_CREAT) {
        if (mode && (uint32_t)mode + 4 <= ctx->memory_size)
            real_mode = *(int32_t *)(ctx->memory + (uint32_t)mode);
    } else {
        real_mode = 0;
    }
    int r = openat(host_dfd, path, hflags, real_mode);
    if (ydebug_enabled())
        ydebug("openat(dfd=%d \"%s\" flags=0x%x->0x%x mode_off=%d real_mode=0%o) = %d%s\n",
               host_dfd, path, flags, hflags, mode, real_mode, r,
               r < 0 ? strerror(errno) : "");
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
