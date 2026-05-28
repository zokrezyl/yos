/* Windows host impls of the platform abstraction.
 *
 * Built only when host_machine.system() == 'windows'. Linux/darwin
 * have their own translation units in platform/posix/. */

#include "platform.h"

#include <errno.h>
#include <io.h>          /* _get_osfhandle */
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

pid_t yos_plat_gettid(void)
{
    return (pid_t)GetCurrentThreadId();
}

#define YOS_FADV_NORMAL      0
#define YOS_FADV_RANDOM      1
#define YOS_FADV_SEQUENTIAL  2
#define YOS_FADV_WILLNEED    3
#define YOS_FADV_DONTNEED    4
#define YOS_FADV_NOREUSE     5

int yos_plat_posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
    (void)fd; (void)offset; (void)len;
    switch (advice) {
    case YOS_FADV_NORMAL:
    case YOS_FADV_RANDOM:
    case YOS_FADV_SEQUENTIAL:
    case YOS_FADV_WILLNEED:
    case YOS_FADV_DONTNEED:
    case YOS_FADV_NOREUSE:
        return 0;
    default:
        return EINVAL;
    }
}

int yos_plat_posix_fallocate(int fd, off_t offset, off_t len)
{
    if (offset < 0 || len <= 0) return EINVAL;

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return EBADF;

    LARGE_INTEGER cur_size;
    if (!GetFileSizeEx(h, &cur_size)) return EIO;

    int64_t need_end = (int64_t)offset + (int64_t)len;
    if (need_end <= cur_size.QuadPart) return 0;

    /* Remember current position so we can restore it after the zero-fill. */
    LARGE_INTEGER zero = {0};
    LARGE_INTEGER saved_pos;
    if (!SetFilePointerEx(h, zero, &saved_pos, FILE_CURRENT)) return EIO;

    /* Seek to old EOF and write zeros up to need_end so the new range
     * is logically allocated and zero-filled per POSIX. */
    LARGE_INTEGER seek;
    seek.QuadPart = cur_size.QuadPart;
    if (!SetFilePointerEx(h, seek, NULL, FILE_BEGIN)) return EIO;

    unsigned char zeros[65536];
    memset(zeros, 0, sizeof zeros);
    int64_t left = need_end - cur_size.QuadPart;
    while (left > 0) {
        DWORD chunk = (left > (int64_t)sizeof zeros)
                          ? (DWORD)sizeof zeros : (DWORD)left;
        DWORD wrote = 0;
        if (!WriteFile(h, zeros, chunk, &wrote, NULL) || wrote != chunk) {
            int e = (GetLastError() == ERROR_DISK_FULL) ? ENOSPC : EIO;
            SetFilePointerEx(h, saved_pos, NULL, FILE_BEGIN);
            return e;
        }
        left -= chunk;
    }

    SetFilePointerEx(h, saved_pos, NULL, FILE_BEGIN);
    return 0;
}

void yos_plat_exit(int code)
{
    /* ExitProcess is Win32's terminate-and-go: no atexit, no destructor
     * cascade, no DllMain on threads — just emit the exit code and
     * release the process record. Avoids the debug-CRT hang we hit
     * during FLS-callback / worker-thread teardown. */
    ExitProcess((UINT)code);
}

/* Cheap socket-vs-CRT-fd classifier. getsockopt(SO_TYPE) succeeds only
 * on a real Winsock SOCKET handle; on a CRT-managed file descriptor it
 * returns WSAENOTSOCK. */
static int yos_is_socket_fd(int hfd)
{
    int t = 0; int len = (int)sizeof t;
    return getsockopt((SOCKET)hfd, SOL_SOCKET, SO_TYPE,
                      (char *)&t, &len) == 0;
}

int yos_plat_close(int hfd)
{
    if (yos_is_socket_fd(hfd)) {
        return closesocket((SOCKET)hfd) == 0 ? 0 : -1;
    }
    return _close(hfd);
}

ssize_t yos_plat_read(int hfd, void *buf, size_t n)
{
    if (yos_is_socket_fd(hfd)) {
        int r = recv((SOCKET)hfd, (char *)buf, (int)n, 0);
        return (ssize_t)r;
    }
    return (ssize_t)_read(hfd, buf, (unsigned)n);
}

ssize_t yos_plat_write(int hfd, const void *buf, size_t n)
{
    if (yos_is_socket_fd(hfd)) {
        int r = send((SOCKET)hfd, (const char *)buf, (int)n, 0);
        return (ssize_t)r;
    }
    int r = _write(hfd, buf, (unsigned)n);
    if (r < 0) {
        HANDLE h = (HANDLE)_get_osfhandle(hfd);
        if (h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_PIPE) {
            /* MSVC's host errno=32 (EPIPE) collides with ECHOK in the
             * codegen's constants_remap, so setting errno=32 would
             * remap to FreeBSD 4 instead of FreeBSD 32. MSVC's ECHOPRT
             * value (1024) has a unique remap path to FreeBSD EPIPE
             * (32) — use it as our sentinel. */
            errno = 1024;
        }
    }
    return (ssize_t)r;
}

int yos_plat_isatty(int hfd)
{
    if (yos_is_socket_fd(hfd)) return 0;
    return _isatty(hfd);
}

int yos_plat_access(const char *path, int mode)
{
    /* POSIX X_OK (1) isn't accepted by msvcrt's _access — strip it.
     * F_OK (0), R_OK (4), W_OK (2) all pass through cleanly. */
    return _access(path, mode & 0x6);
}

/* Forward decls for the per-fd flag table maintained in compat_libc.c. */
extern void yos_fdflags_record_open(int fd, int flags);
extern void yos_fdmode_record(int fd, int mode);
extern int  yos_fdmode_get(int fd);

int yos_plat_open(const char *path, int flags, int mode)
{
    int fd = open(path, flags, mode);
    if (fd >= 0) {
        yos_fdflags_record_open(fd, flags);
        /* Stamp the POSIX mode for files we created — yos_plat_fstat
         * splices it back into st_mode so umask-then-fstat round-trips
         * across the Windows filesystem (which only persists the
         * readonly attribute). */
        if ((flags & 0x100 /* _O_CREAT */) != 0 && mode != 0) {
            yos_fdmode_record(fd, mode);
        }
    }
    return fd;
}

const char *yos_plat_translate_path(const char *path)
{
    /* Per-thread scratch so concurrent forks don't clobber each other.
     * 4 KiB covers any reasonable %TEMP% concatenation. */
    static __declspec(thread) char buf[4096];
    if (!path) return NULL;

    /* Exact-match POSIX device names → Windows null/console devices. */
    if (strcmp(path, "/dev/null") == 0)         return "NUL";
    if (strcmp(path, "/dev/zero") == 0)         return "NUL";
    if (strcmp(path, "/dev/random") == 0)       return "NUL";
    if (strcmp(path, "/dev/urandom") == 0)      return "NUL";
    if (strcmp(path, "/dev/tty") == 0)          return "CON";
    if (strcmp(path, "/dev/stdin") == 0)        return "CONIN$";
    if (strcmp(path, "/dev/stdout") == 0)       return "CONOUT$";
    if (strcmp(path, "/dev/stderr") == 0)       return "CONOUT$";

    /* /tmp/<x> and /tmp on their own → %TEMP%\<x>. */
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
        (path[4] == 0 || path[4] == '/')) {
        const char *tmpdir = getenv("TEMP");
        if (!tmpdir) tmpdir = getenv("TMP");
        if (!tmpdir) tmpdir = "C:\\Windows\\Temp";
        snprintf(buf, sizeof buf, "%s%s", tmpdir, path + 4);
        return buf;
    }

    /* "/" alone → %SystemDrive%\ (typically C:\). Anchors guest's
     * absolute paths to the system drive so opendir("/"), stat("/"),
     * etc. resolve against a real directory. */
    if (path[0] == '/' && path[1] == 0) {
        const char *sd = getenv("SystemDrive");
        if (!sd) sd = "C:";
        snprintf(buf, sizeof buf, "%s\\", sd);
        return buf;
    }

    return path;
}

int yos_plat_fstat(int hfd, struct stat *out)
{
    if (!out) { errno = EFAULT; return -1; }

    /* yos_fd_alloc on Windows packs both CRT file descriptors and
     * Winsock SOCKETs into the host_fd table. SOCKETs are HANDLE-ish
     * kernel objects (just integers); _fstat64i32 on them is undefined.
     * Detect via getsockopt(SO_TYPE) — succeeds only on a valid socket
     * — and synthesise an S_IFSOCK stat. Otherwise fall through to the
     * CRT's fstat. */
    int sock_type = 0;
    int len = (int)sizeof sock_type;
    if (getsockopt((SOCKET)hfd, SOL_SOCKET, SO_TYPE,
                   (char *)&sock_type, &len) == 0) {
        memset(out, 0, sizeof *out);
        out->st_mode = _S_IFIFO | 0666;  /* slot present so caller knows */
        /* S_IFSOCK isn't in MSVC's <sys/stat.h>; we defined the value
         * 0140000 in posix_extras.h. Apply it directly to st_mode here
         * so S_ISSOCK(st_mode) on the wasm side reads as true. */
        out->st_mode = (unsigned short)((out->st_mode & ~0170000u) | 0140000u);
        return 0;
    }

    int rc = _fstat64i32(hfd, (struct _stat64i32 *)out);
    if (rc == 0) {
        /* Splice the recorded POSIX mode (umask-masked open mode) into
         * the permission bits — Windows persists only the read-only
         * attribute, so a fstat of a freshly-created file otherwise
         * always reads back 0666. */
        int recorded = yos_fdmode_get(hfd);
        if (recorded != 0) {
            out->st_mode = (unsigned short)((out->st_mode & ~0777u) |
                                            (recorded & 0777u));
        }
    }
    return rc;
}
