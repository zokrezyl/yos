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
 * Wasm-side `DIR *` is a wasm offset to an 8-byte struct in linear
 * memory:
 *     +0 : int32_t  dd_fd     — wasm fd; FreeBSD libc's `_dirfd(dirp)`
 *                               macro (from gen-private.h) is
 *                               `((dirp)->dd_fd)` — i.e. a load of the
 *                               first int. fts/find/ls all rely on
 *                               that path returning a usable fd.
 *     +4 : uint32_t slot_idx  — index back into g_dirs[]. Bridges
 *                               look the slot up here so we can keep
 *                               the per-stream state host-side without
 *                               teaching the guest about it.
 *
 * Returning a real wasm pointer (not a 1..63 slot index) is what makes
 * `_dirfd` work — fts_safe_changedir uses `_fstat(_dirfd(dirp))` to
 * verify it's still pointing at the directory it expected, and an
 * arbitrary garbage read at offset slot_index would compare against
 * stack data or random heap.
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
#include <unistd.h>

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
    uint32_t dd_off;               /* wasm offset of the _dirdesc-shaped
                                      header (the value we return to the
                                      guest as DIR*). 0 when unset. */
    int32_t  wasm_fd;              /* wasm fd written into dd_off+0. -1
                                      for vfs-backed dirs with no host
                                      fd to surface. */

    /* virtual-fs read-ahead buffer (host-side) */
    uint8_t  vfs_buf[YOS_VFS_DIRBUF_SIZE];
    size_t   vfs_buf_len;          /* bytes valid in vfs_buf */
    size_t   vfs_buf_pos;          /* next entry offset to consume */
    int      vfs_exhausted;        /* getdents64 returned 0 */
};

/* Header laid into wasm linear memory at slot->dd_off. */
#define YOS_DD_SIZE         8
#define YOS_DD_FD_OFF       0
#define YOS_DD_SLOTIDX_OFF  4

static struct yos_dir_slot g_dirs[YOS_DIR_MAX];
static pthread_mutex_t     g_dirs_lock = PTHREAD_MUTEX_INITIALIZER;

/* Look up a slot by its yos-internal index (used only when we already
 * know the slot — see slot_from_dd for the guest-pointer path). */
static struct yos_dir_slot *slot_get(uint32_t handle)
{
    if (handle == 0 || handle >= YOS_DIR_MAX) return NULL;
    if (!g_dirs[handle].in_use) return NULL;
    return &g_dirs[handle];
}

/* Translate a guest DIR* (wasm offset of the header struct) to its
 * yos slot. Returns NULL on any inconsistency — bridges then surface
 * EBADF, matching what libc would do for a stale DIR pointer. */
static struct yos_dir_slot *slot_from_dd(struct yos_exec_ctx *ctx,
                                         uint32_t dd_off)
{
    if (dd_off == 0) return NULL;
    if ((uint64_t)dd_off + YOS_DD_SIZE > ctx->memory_size) return NULL;
    uint32_t idx = *(uint32_t *)(ctx->memory + dd_off + YOS_DD_SLOTIDX_OFF);
    struct yos_dir_slot *slot = slot_get(idx);
    if (!slot || slot->dd_off != dd_off) return NULL;
    return slot;
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
            g_dirs[i].wasm_fd = -1;
            pthread_mutex_unlock(&g_dirs_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_dirs_lock);
    return 0;
}

/* Allocate the per-slot wasm scratch (FreeBSD dirent buffer) AND the
 * 8-byte header struct the guest will see as DIR*. The header carries
 * dd_fd at +0 (so libc's `_dirfd(dirp)` macro returns a usable wasm
 * fd) and our slot index at +4 (so the bridges can find the slot
 * again from just the guest pointer). */
static int slot_finalise(struct yos_exec_ctx *ctx, uint32_t handle)
{
    struct yos_dir_slot *slot = &g_dirs[handle];
    slot->scratch_off = yos_malloc(ctx, YOS_FBSD_DIRENT_SIZE);
    if (slot->scratch_off == 0) goto fail;
    slot->dd_off = yos_malloc(ctx, YOS_DD_SIZE);
    if (slot->dd_off == 0) {
        yos_free(ctx, slot->scratch_off);
        slot->scratch_off = 0;
        goto fail;
    }
    *(int32_t  *)(ctx->memory + slot->dd_off + YOS_DD_FD_OFF) = slot->wasm_fd;
    *(uint32_t *)(ctx->memory + slot->dd_off + YOS_DD_SLOTIDX_OFF) = handle;
    return 0;
fail:
    pthread_mutex_lock(&g_dirs_lock);
    slot->in_use = 0;
    pthread_mutex_unlock(&g_dirs_lock);
    return -1;
}

/* Procfs file table is per-ctx (procfs_fds). Look up the table and
 * the associated file struct for a given virtual fd. */
static struct yos_file *vfs_file_from_fd(struct yos_exec_ctx *ctx, int32_t fd)
{
    struct yos_file_table *ft = (struct yos_file_table *)ctx->procfs_fds;
    if (!ft) return NULL;
    return yos_file_get(ft, fd);
}

/* Assign a wasm fd that resolves to the host fd backing a freshly-
 * opened DIR*. dup()'d so closedir() and yos_close(wasm_fd) can each
 * release their half independently (closedir owns the original, the
 * wasm fd owns the dup). Returns -1 on failure (caller must close the
 * DIR* itself in that case). */
static int alloc_wasm_fd_for_host_dir(struct yos_exec_ctx *ctx, DIR *d)
{
    extern int yos_fd_alloc(struct yos_exec_ctx *, int);
    int orig = dirfd(d);
    if (orig < 0) return -1;
    int duped = dup(orig);
    if (duped < 0) return -1;
    int wfd = yos_fd_alloc(ctx, duped);
    if (wfd < 0) { close(duped); return -1; }
    return wfd;
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
        g_dirs[h].wasm_fd  = vfd; /* virtual fd; usable for fstat etc. */
        if (slot_finalise(ctx, h) < 0) {
            if (ops->close) ops->close(ctx, vfile);
            return yos_errno_null(ctx, ENOMEM);
        }
        if (ydebug_enabled())
            ydebug("opendir(\"%s\") = dd_off 0x%x slot %u wasm_fd %d (vfs)\n",
                   path, g_dirs[h].dd_off, h, g_dirs[h].wasm_fd);
        return g_dirs[h].dd_off;
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
    g_dirs[h].wasm_fd  = alloc_wasm_fd_for_host_dir(ctx, d);
    if (slot_finalise(ctx, h) < 0) {
        closedir(d);
        return yos_errno_null(ctx, ENOMEM);
    }
    return g_dirs[h].dd_off;
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
    /* fdopendir transfers ownership of the host fd to the DIR — the
     * caller's wasm fd still refers to the same host fd, which is what
     * we want surfaced via _dirfd. No dup needed; the wasm fd already
     * points at it. */
    g_dirs[h].wasm_fd = wasm_fd;
    if (slot_finalise(ctx, h) < 0) {
        closedir(d);
        return yos_errno_null(ctx, ENOMEM);
    }
    return g_dirs[h].dd_off;
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

uint32_t yos_readdir(struct yos_exec_ctx *ctx, uint32_t dd_off)
{
    struct yos_dir_slot *slot = slot_from_dd(ctx, dd_off);
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

int32_t yos_closedir(struct yos_exec_ctx *ctx, uint32_t dd_off)
{
    struct yos_dir_slot *slot = slot_from_dd(ctx, dd_off);
    if (!slot) return yos_errno_neg(ctx, EBADF);

    int rc = 0;
    if (slot->host_dir) {
        if (closedir(slot->host_dir) < 0) rc = -errno;
    } else if (slot->vfs_file && slot->vfs_ops && slot->vfs_ops->close) {
        int32_t r = slot->vfs_ops->close(ctx, slot->vfs_file);
        if (r < 0) rc = r;
    }
    uint32_t scratch  = slot->scratch_off;
    uint32_t hdr_off  = slot->dd_off;
    pthread_mutex_lock(&g_dirs_lock);
    memset(slot, 0, sizeof(*slot));
    slot->wasm_fd = -1;
    pthread_mutex_unlock(&g_dirs_lock);
    if (scratch) yos_free(ctx, scratch);
    if (hdr_off) yos_free(ctx, hdr_off);
    if (rc < 0) return yos_errno_neg(ctx, -rc);
    return 0;
}

int32_t yos_dirfd(struct yos_exec_ctx *ctx, uint32_t dd_off)
{
    /* Now trivial: the wasm fd already lives at dd_off+0 so the libc
     * macro `_dirfd(dirp)` returns it without an env import. Bridges
     * are still called by anything that uses `dirfd(dirp)` as an
     * actual function — give them the same answer. */
    struct yos_dir_slot *slot = slot_from_dd(ctx, dd_off);
    if (!slot) return yos_errno_neg(ctx, EBADF);
    if (slot->wasm_fd < 0) return yos_errno_neg(ctx, EBADF);
    return slot->wasm_fd;
}

void yos_rewinddir(struct yos_exec_ctx *ctx, uint32_t dd_off)
{
    struct yos_dir_slot *slot = slot_from_dd(ctx, dd_off);
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
