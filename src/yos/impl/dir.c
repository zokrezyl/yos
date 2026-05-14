/* impl/dir.c — directory-stream bridge.
 *
 * Two backends behind one DIR* handle table:
 *
 *   1. Host-backed (regular filesystem paths). Each slot owns a host
 *      glibc DIR*. readdir converts host glibc dirent → FreeBSD-i386
 *      struct dirent into a per-DIR scratch buffer in wasm linear
 *      memory and returns the wasm offset.
 *
 *   2. Virtual-FS-backed (paths that hit yos's mount table — e.g.
 *      /proc). Each slot owns a `struct yos_file *` plus the matching
 *      ops vtable. readdir calls `ops->getdents64` to fill a Linux-
 *      style getdents buffer, then walks one Linux dirent at a time
 *      and lays out the FreeBSD-i386 form into the wasm scratch. This
 *      is how ps(1) and any /proc reader gets yos's process table
 *      instead of the host's.
 *
 * Wasm-side `DIR *` is a small int (1..YOS_DIR_MAX-1); slot 0 is the
 * canonical NULL so `if ((dirp = opendir(...)) == NULL)` keeps
 * working.
 *
 * Forks share the table (same global). Same compromise as
 * impl/file.c's FILE* table — directory streams rarely outlive a fork
 * boundary in practice. Lock to keep the slot allocator race-free.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>

#include "yos/types.h"
#include "yos/ydebug.h"
#include "impl/alloc.h"
#include "impl/errno_helpers.h"
#include "vfs/mount.h"
#include "vfs/file.h"

#define YOS_DIR_MAX 64

/* FreeBSD-i386 `struct dirent`:
 *
 *   offset  size  field
 *      0     8    d_fileno   (ino_t = uint64_t)
 *      8     8    d_off      (off_t = int64_t)
 *     16     2    d_reclen   (uint16_t)
 *     18     1    d_type     (uint8_t)
 *     19     1    d_pad0     (uint8_t)
 *     20     2    d_namlen   (uint16_t)
 *     22     2    d_pad1     (uint16_t)
 *     24   256    d_name[MAXNAMLEN+1]
 *
 * MAXNAMLEN is 255. Total: 24 + 256 = 280 bytes. */
#define YOS_FBSD_DIRENT_NAME_OFF  24
#define YOS_FBSD_DIRENT_SIZE      280

/* Linux getdents64 record layout (matches yos/vfs/procfs.c's writer):
 *
 *   offset  size  field
 *      0     8    d_ino     (uint64_t)
 *      8     8    d_off     (uint64_t)        — really record length
 *     16     2    d_reclen  (uint16_t)
 *     18     1    d_type    (uint8_t)
 *     19   ...    d_name    (NUL-terminated)
 */
#define YOS_LX_DIRENT_NAME_OFF    19

/* Per-DIR linux-style getdents buffer. Big enough for ~16 entries of
 * typical name lengths. yos's procfs root caps at 'self' + 'mounts'
 * + active PIDs; one batch is fine for now. */
#define YOS_VFS_DIRBUF_SIZE       4096

struct yos_dir_slot {
    /* exactly one of host_dir / vfs_file is non-NULL when in_use=1 */
    int in_use;
    DIR *host_dir;
    struct yos_file *vfs_file;
    const struct yos_file_operations *vfs_ops;

    uint32_t scratch_off;          /* wasm offset of FreeBSD dirent buffer */

    /* virtual-fs read-ahead buffer (host-side) */
    uint8_t  vfs_buf[YOS_VFS_DIRBUF_SIZE];
    size_t   vfs_buf_len;          /* bytes valid in vfs_buf */
    size_t   vfs_buf_pos;          /* next entry offset to consume */
    int      vfs_exhausted;        /* getdents64 returned 0 */
};

static struct yos_dir_slot g_dirs[YOS_DIR_MAX];
static pthread_mutex_t     g_dirs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct yos_dir_slot *slot_get(uint32_t handle)
{
    if (handle == 0 || handle >= YOS_DIR_MAX) return NULL;
    if (!g_dirs[handle].in_use) return NULL;
    return &g_dirs[handle];
}

/* Reserve a slot; caller fills host_dir / vfs_file then commits with
 * slot_finalise(). Returns 0 on failure. */
static uint32_t slot_reserve(void)
{
    pthread_mutex_lock(&g_dirs_lock);
    for (uint32_t i = 1; i < YOS_DIR_MAX; i++) {
        if (!g_dirs[i].in_use) {
            memset(&g_dirs[i], 0, sizeof(g_dirs[i]));
            g_dirs[i].in_use = 1;
            pthread_mutex_unlock(&g_dirs_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_dirs_lock);
    return 0;
}

static int slot_finalise(struct yos_exec_ctx *ctx, uint32_t handle)
{
    g_dirs[handle].scratch_off = yos_malloc(ctx, YOS_FBSD_DIRENT_SIZE);
    if (g_dirs[handle].scratch_off == 0) {
        pthread_mutex_lock(&g_dirs_lock);
        g_dirs[handle].in_use = 0;
        pthread_mutex_unlock(&g_dirs_lock);
        return -1;
    }
    return 0;
}

/* Procfs file table is per-ctx (procfs_fds). Look up the table and
 * the associated file struct for a given virtual fd. */
static struct yos_file *vfs_file_from_fd(struct yos_exec_ctx *ctx, int32_t fd)
{
    struct yos_file_table *ft = (struct yos_file_table *)ctx->procfs_fds;
    if (!ft) return NULL;
    return yos_file_get(ft, fd);
}

uint32_t yos_opendir(struct yos_exec_ctx *ctx, uint32_t path_off)
{
    if (path_off == 0 || path_off >= ctx->memory_size)
        return yos_errno_null(ctx, EFAULT);
    const char *path = (const char *)(ctx->memory + path_off);

    /* Mount-table lookup: if the path lands in a virtual filesystem
     * (today: /proc), route through it instead of opening on the host.
     * Without this, ps(1) and any other /proc reader would see the
     * host's /proc — completely unrelated processes. */
    struct yos_mount_table *mt =
        (struct yos_mount_table *)ctx->rt->mount_table;
    const char *remaining = NULL;
    const struct yos_file_operations *ops =
        mt ? yos_mount_resolve(mt, path, &remaining) : NULL;

    if (ops && ops->open && ops->getdents64) {
        /* Open through the virtual filesystem. The vfs allocates a
         * yos_file slot in the per-ctx file table and returns a
         * virtual fd (>= YOS_VFS_FD_BASE). */
        int32_t vfd = ops->open(ctx, remaining ? remaining : "",
                                /* flags  */ 0,
                                /* mode   */ 0);
        if (vfd < 0) return yos_errno_null(ctx, -vfd);
        struct yos_file *vfile = vfs_file_from_fd(ctx, vfd);
        if (!vfile) return yos_errno_null(ctx, ENOENT);

        uint32_t h = slot_reserve();
        if (h == 0) {
            if (ops->close) ops->close(ctx, vfile);
            return yos_errno_null(ctx, EMFILE);
        }
        g_dirs[h].vfs_file = vfile;
        g_dirs[h].vfs_ops  = ops;
        if (slot_finalise(ctx, h) < 0) {
            if (ops->close) ops->close(ctx, vfile);
            return yos_errno_null(ctx, ENOMEM);
        }
        if (ydebug_enabled())
            ydebug("opendir(\"%s\") = handle %u (vfs)\n", path, h);
        return h;
    }

    /* Host-backed path. */
    errno = 0;
    DIR *d = opendir(path);
    if (ydebug_enabled())
        ydebug("opendir(\"%s\") = %p%s\n", path, (void *)d,
               d ? "" : strerror(errno));
    if (!d) return yos_errno_null(ctx, errno);
    uint32_t h = slot_reserve();
    if (h == 0) { closedir(d); return yos_errno_null(ctx, EMFILE); }
    g_dirs[h].host_dir = d;
    if (slot_finalise(ctx, h) < 0) {
        closedir(d);
        return yos_errno_null(ctx, ENOMEM);
    }
    return h;
}

uint32_t yos_fdopendir(struct yos_exec_ctx *ctx, int32_t wasm_fd)
{
    extern int yos_fd_get(struct yos_exec_ctx *, int);
    int hfd = yos_fd_get(ctx, wasm_fd);
    if (hfd < 0) return yos_errno_null(ctx, EBADF);
    errno = 0;
    DIR *d = fdopendir(hfd);
    if (!d) return yos_errno_null(ctx, errno);
    uint32_t h = slot_reserve();
    if (h == 0) { closedir(d); return yos_errno_null(ctx, EMFILE); }
    g_dirs[h].host_dir = d;
    if (slot_finalise(ctx, h) < 0) {
        closedir(d);
        return yos_errno_null(ctx, ENOMEM);
    }
    return h;
}

/* Lay one FreeBSD-i386 dirent into the slot's wasm scratch and return
 * its wasm offset. Caller supplies the fields. */
static uint32_t emit_fbsd_dirent(struct yos_exec_ctx *ctx,
                                 struct yos_dir_slot *slot,
                                 uint64_t fileno_v, int64_t off_v,
                                 uint8_t type, const char *name,
                                 size_t namlen)
{
    if (namlen > 255) namlen = 255;
    uint8_t *dst = ctx->memory + slot->scratch_off;
    memset(dst, 0, YOS_FBSD_DIRENT_SIZE);

    memcpy(dst + 0,  &fileno_v, 8);
    memcpy(dst + 8,  &off_v,    8);
    uint16_t reclen = (uint16_t)((YOS_FBSD_DIRENT_NAME_OFF + namlen + 1 + 7) & ~7u);
    memcpy(dst + 16, &reclen,   2);
    dst[18] = type;
    uint16_t namlen_v = (uint16_t)namlen;
    memcpy(dst + 20, &namlen_v, 2);
    memcpy(dst + YOS_FBSD_DIRENT_NAME_OFF, name, namlen);
    return slot->scratch_off;
}

uint32_t yos_readdir(struct yos_exec_ctx *ctx, uint32_t handle)
{
    struct yos_dir_slot *slot = slot_get(handle);
    if (!slot) return yos_errno_null(ctx, EBADF);

    if (slot->vfs_file) {
        /* Virtual-FS path: pull one Linux dirent out of our read-
         * ahead buffer; refill from getdents64 when exhausted. */
        for (;;) {
            if (slot->vfs_buf_pos < slot->vfs_buf_len) {
                uint8_t *e = slot->vfs_buf + slot->vfs_buf_pos;
                uint16_t reclen;
                memcpy(&reclen, e + 16, 2);
                if (reclen == 0 ||
                    slot->vfs_buf_pos + reclen > slot->vfs_buf_len) {
                    /* corrupt or truncated — bail and try refill. */
                    slot->vfs_buf_pos = slot->vfs_buf_len;
                    continue;
                }
                uint64_t ino;   memcpy(&ino,   e + 0, 8);
                uint64_t off_v; memcpy(&off_v, e + 8, 8);
                uint8_t  type = e[18];
                const char *name = (const char *)(e + YOS_LX_DIRENT_NAME_OFF);
                size_t namlen = strlen(name);
                slot->vfs_buf_pos += reclen;
                return emit_fbsd_dirent(ctx, slot, ino, (int64_t)off_v,
                                        type, name, namlen);
            }
            if (slot->vfs_exhausted) return 0;
            int32_t n = slot->vfs_ops->getdents64(
                ctx, slot->vfs_file, slot->vfs_buf, YOS_VFS_DIRBUF_SIZE);
            if (n < 0) return yos_errno_null(ctx, -n);
            if (n == 0) { slot->vfs_exhausted = 1; return 0; }
            slot->vfs_buf_len = (size_t)n;
            slot->vfs_buf_pos = 0;
        }
    }

    /* Host-backed path. */
    errno = 0;
    struct dirent *e = readdir(slot->host_dir);
    if (!e) {
        if (errno) return yos_errno_null(ctx, errno);
        return 0;
    }
    size_t namlen = strlen(e->d_name);
#if defined(__APPLE__)
    /* darwin spells the seek-offset field d_seekoff; same semantics as
     * Linux d_off — opaque cookie to seekdir back to this entry. */
    int64_t seekoff = (int64_t)e->d_seekoff;
#else
    int64_t seekoff = (int64_t)e->d_off;
#endif
    return emit_fbsd_dirent(ctx, slot,
                            (uint64_t)e->d_ino, seekoff,
                            (uint8_t)e->d_type, e->d_name, namlen);
}

int32_t yos_closedir(struct yos_exec_ctx *ctx, uint32_t handle)
{
    struct yos_dir_slot *slot = slot_get(handle);
    if (!slot) return yos_errno_neg(ctx, EBADF);

    int rc = 0;
    if (slot->host_dir) {
        if (closedir(slot->host_dir) < 0) rc = -errno;
    } else if (slot->vfs_file && slot->vfs_ops && slot->vfs_ops->close) {
        int32_t r = slot->vfs_ops->close(ctx, slot->vfs_file);
        if (r < 0) rc = r;
    }
    uint32_t scratch = slot->scratch_off;
    pthread_mutex_lock(&g_dirs_lock);
    memset(slot, 0, sizeof(*slot));
    pthread_mutex_unlock(&g_dirs_lock);
    if (scratch) yos_free(ctx, scratch);
    if (rc < 0) return yos_errno_neg(ctx, -rc);
    return 0;
}

int32_t yos_dirfd(struct yos_exec_ctx *ctx, uint32_t handle)
{
    extern int yos_fd_alloc(struct yos_exec_ctx *, int);
    extern int yos_fd_get  (struct yos_exec_ctx *, int);

    struct yos_dir_slot *slot = slot_get(handle);
    if (!slot) return yos_errno_neg(ctx, EBADF);

    /* Virtual DIRs have no host fd to hand out — yos_file is opaque.
     * Returning -EBADF tells the caller (fts.c, find, …) it can't
     * fchdir into it; they fall back to chdir(path). */
    if (!slot->host_dir) return yos_errno_neg(ctx, EBADF);

    int hfd = dirfd(slot->host_dir);
    if (hfd < 0) return yos_errno_neg(ctx, errno);
    for (int wfd = 0; wfd < 256; wfd++) {
        if (yos_fd_get(ctx, wfd) == hfd) return wfd;
    }
    int wfd = yos_fd_alloc(ctx, hfd);
    if (wfd < 0) return yos_errno_neg(ctx, EMFILE);
    return wfd;
}

void yos_rewinddir(struct yos_exec_ctx *ctx, uint32_t handle)
{
    (void)ctx;
    struct yos_dir_slot *slot = slot_get(handle);
    if (!slot) return;
    if (slot->host_dir) {
        rewinddir(slot->host_dir);
    } else if (slot->vfs_file) {
        slot->vfs_file->dir_index = 0;
        slot->vfs_buf_len = slot->vfs_buf_pos = 0;
        slot->vfs_exhausted = 0;
    }
}

/* copy_file_range(2): Linux-only kernel fast-path. cp(1) probes it
 * first and only falls back to read/write on EINVAL — the bridge
 * generator's generic stub returns ENOSYS, which sent cp into a
 * "Remote address changed" warn-and-give-up path with zero-byte
 * destinations. Returning -EINVAL here keeps cp on the portable
 * fallback (and matches what would happen on a host filesystem
 * that genuinely refuses copy_file_range). When we want zero-copy
 * for real, replace this with a host copy_file_range call gated on
 * __linux__. */
int32_t yos_copy_file_range(struct yos_exec_ctx *ctx,
                            int32_t fd_in,  uint32_t off_in_ptr,
                            int32_t fd_out, uint32_t off_out_ptr,
                            uint32_t len, uint32_t flags)
{
    (void)fd_in; (void)off_in_ptr; (void)fd_out;
    (void)off_out_ptr; (void)len; (void)flags;
    return yos_errno_neg(ctx, EINVAL);
}

/* poll(2): walk a wasm array of pollfd, translate each fd through
 * fd_map to a host fd, call host poll, copy revents back. The
 * pollfd layout is identical on FreeBSD-i386 and host Linux/glibc
 * (int + short + short = 8 bytes, both ends), so we can call host
 * poll directly with the translated array. POLL* event-bit values
 * also match between FreeBSD and Linux for the bits zsh/ZLE cares
 * about (POLLIN/OUT/ERR/HUP/NVAL/PRI), so no flag remap.
 *
 * Without this bridge, zsh ZLE's getbyte() traps with
 *   yos: unresolved import env.poll
 * the first time it peeks for an escape-sequence continuation —
 * which is also the path that turns plain backspace into the
 * "advances like space" symptom because zsh falls out of ZLE mid-
 * edit and the line driver echoes through canonical mode oddly.
 */
#include <poll.h>
int32_t yos_poll(struct yos_exec_ctx *ctx,
                 uint32_t pfds_off, uint32_t nfds, int32_t timeout)
{
    extern int yos_fd_get(struct yos_exec_ctx *, int);

    if (nfds == 0) return (int32_t)poll(NULL, 0, timeout);
    if (nfds > 1024) return yos_errno_neg(ctx, EINVAL);
    if (pfds_off == 0 || pfds_off + nfds * 8 > ctx->memory_size)
        return yos_errno_neg(ctx, EFAULT);

    struct pollfd host_pfds[1024];
    uint8_t *w = ctx->memory + pfds_off;
    for (uint32_t i = 0; i < nfds; i++) {
        int32_t wfd;     memcpy(&wfd,    w + i*8 + 0, 4);
        int16_t events;  memcpy(&events, w + i*8 + 4, 2);
        int hfd = (wfd >= 0) ? yos_fd_get(ctx, wfd) : wfd;
        /* A negative wfd is a deliberate "ignore this slot" sentinel
         * per POSIX. Pass it through so host poll skips the entry. */
        host_pfds[i].fd      = (wfd < 0) ? wfd : (hfd < 0 ? -1 : hfd);
        host_pfds[i].events  = events;
        host_pfds[i].revents = 0;
    }

    int r = poll(host_pfds, (nfds_t)nfds, timeout);
    int saved = errno;

    for (uint32_t i = 0; i < nfds; i++) {
        int16_t revents = (int16_t)host_pfds[i].revents;
        memcpy(w + i*8 + 6, &revents, 2);
    }
    if (r < 0) return yos_errno_neg(ctx, saved);
    return (int32_t)r;
}
