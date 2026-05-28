/* impl/proc/sig-windows.c — Windows-host signal-wait stubs.
 *
 * Windows has no POSIX sigwaitinfo / sigtimedwait. The wasm guest's
 * libuv-style code paths that probe for these get a clean ENOSYS so
 * they can fall back to non-signal-driven event loops (kqueue on
 * Windows is also stubbed; libuv builds for the FreeBSD-shaped guest
 * never reach into either of these on Windows by design).
 *
 * NO #ifdef in this file — meson selects it only on windows hosts.
 */

#include "yos/types.h"
#include "impl/errno_helpers.h"

#include <errno.h>
#include <stdint.h>

int32_t yos_sigwaitinfo(struct yos_exec_ctx *ctx,
                        uint32_t set_off, uint32_t info_off)
{
    (void)set_off; (void)info_off;
    return yos_errno_neg(ctx, ENOSYS);
}

int32_t yos_sigtimedwait(struct yos_exec_ctx *ctx,
                         uint32_t set_off, uint32_t info_off,
                         uint32_t timeout_off)
{
    (void)set_off; (void)info_off; (void)timeout_off;
    return yos_errno_neg(ctx, ENOSYS);
}
