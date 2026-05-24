#ifndef YOS_IO_INTERNAL_H
#define YOS_IO_INTERNAL_H

/* impl/io/io-internal.h — helpers shared between io.c and the per-
 * platform io-<os>.c slices. Not part of the yos public API; not
 * visible outside impl/io/.
 *
 * The "io" naming refers to the POSIX I/O bridges (open / read /
 * write / close + fd_map + dir + FILE* + FIFO + PTY emulation). NOT
 * the same thing as the custom VFS layer in src/yos/vfs/, which owns
 * the mount table + procfs synth backend. impl/io/io.c dispatches to
 * src/yos/vfs/ for /proc paths; the dependency goes io → vfs, never
 * the other way. */

#include "yos/types.h"
#include <stdint.h>

extern int32_t yos_fd_translate(struct yos_exec_ctx *ctx, int32_t fd);
extern int32_t yos_fd_get      (struct yos_exec_ctx *ctx, int32_t wfd);
extern int32_t yos_fd_alloc    (struct yos_exec_ctx *ctx, int host_fd);
extern int32_t yos_fd_close    (struct yos_exec_ctx *ctx, int32_t wfd);
extern int     yos_xlate_dfd   (struct yos_exec_ctx *ctx, int32_t wfd);
extern const char *yos_path_resolve(struct yos_exec_ctx *ctx, const char *p);
extern void    yos_signal_pump (struct yos_exec_ctx *ctx);

struct iovec;
extern int yos_iovec_w32_to_host(struct yos_exec_ctx *ctx,
                                 uint32_t wasm_iov, int32_t iovcnt,
                                 struct iovec *host_iov);

/* wasm32 offset → host pointer. NULL offset and out-of-bounds both
 * return NULL so callers can EFAULT cleanly. */
static inline void *wptr(struct yos_exec_ctx *ctx, uint32_t offset)
{
    if (offset == 0 || offset >= ctx->memory_size) return 0;
    return ctx->memory + offset;
}

static inline const char *wstr(struct yos_exec_ctx *ctx, uint32_t offset)
{
    return (const char *)wptr(ctx, offset);
}

static inline int32_t host_fd(struct yos_exec_ctx *ctx, int32_t fd)
{
    return yos_fd_translate(ctx, fd);
}

/* ── platform-slice contract ──────────────────────────────────────── */
/* FreeBSD ↔ host ioctl request-number remap. Linux returns a table-
 * mapped Linux ioctl request; darwin returns the input unchanged
 * (BSD-lineage ioctl encoding matches FreeBSD verbatim). Defined in
 * io-linux.c / io-darwin.c respectively. */
uint32_t ioctl_cmd_fb_to_lx(uint32_t cmd);

#endif /* YOS_IO_INTERNAL_H */
