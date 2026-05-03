/* impl/errno_helpers.h — small inline helpers for POSIX-conformant
 * error reporting from custom_<area> impls.
 *
 * The auto-generated bridges already do this dance after every host
 * libc call. Hand-written subsystem impls (impl/vfs.c, impl/proc.c,
 * impl/mem.c, …) used to return `-errno` directly, which broke
 * caller checks like `if (rc == -1) ...` (every POSIX wrapper does
 * this) and meant the FreeBSD `errno` macro never reflected the
 * actual error. These helpers let an impl say
 *
 *     return unlink(s) < 0 ? yos_errno_neg(ctx, errno) : 0;
 *
 * which is one expression, sets the per-ctx errno slot, and returns
 * -1 (POSIX). For getters that return uint or pointer values, see
 * yos_errno_zero() / yos_errno_null().
 */
#ifndef YOS_IMPL_ERRNO_HELPERS_H
#define YOS_IMPL_ERRNO_HELPERS_H

#include <stdint.h>
#include <errno.h>   /* errno is thread-local — must come from glibc */

#include "yos/types.h"

extern int yos_remap_errno_h2g(int host_errno);

/* Write the host errno to the per-ctx slot (so the FreeBSD `errno`
 * macro reads correctly), return -1. Use for int-returning fns. */
static inline int32_t yos_errno_neg(struct yos_exec_ctx *ctx, int host_errno)
{
    if (ctx && ctx->memory && ctx->errno_off) {
        *(int *)(ctx->memory + ctx->errno_off) =
            yos_remap_errno_h2g(host_errno);
    }
    return -1;
}

/* Same but for fns that return 0 (success) / -1 (error). */
static inline int32_t yos_errno_check(struct yos_exec_ctx *ctx, int rc)
{
    if (rc < 0) return yos_errno_neg(ctx, errno);
    return (int32_t)rc;
}

/* For pointer-returning fns: set errno, return 0 (NULL). */
static inline uint32_t yos_errno_null(struct yos_exec_ctx *ctx, int host_errno)
{
    if (ctx && ctx->memory && ctx->errno_off) {
        *(int *)(ctx->memory + ctx->errno_off) =
            yos_remap_errno_h2g(host_errno);
    }
    return 0;
}

#endif /* YOS_IMPL_ERRNO_HELPERS_H */
