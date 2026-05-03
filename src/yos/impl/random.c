/* impl/random.c — yos-side getentropy with proper EFAULT on NULL.
 *
 * The auto-generated bridge for getentropy translated wasm offset 0
 * to `ctx->memory + 0` (start of linear memory) and let host glibc
 * write a byte there — instead of returning -1+EFAULT as POSIX
 * requires. The FreeBSD `getentropy_fault` test pinned this; this
 * file restores the standard behaviour. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>

#include "yos/types.h"
#include "impl/errno_helpers.h"

int32_t yos_getentropy(struct yos_exec_ctx *ctx, uint32_t buf_off, uint32_t len)
{
    if (buf_off == 0)
        return yos_errno_neg(ctx, EFAULT);
    if (len > 256)
        return yos_errno_neg(ctx, EINVAL);
    if (buf_off + len > ctx->memory_size)
        return yos_errno_neg(ctx, EFAULT);
    return yos_errno_check(ctx, getentropy(ctx->memory + buf_off, (size_t)len));
}
