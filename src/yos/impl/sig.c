#include "yos/types.h"
#include "yos/ydebug.h"
#include <stdint.h>
#include <errno.h>

/* Signal handling stubs - we can't actually deliver signals in WASM,
 * but we return success so programs think they registered handlers. */

int32_t yos_sig_rt_sigaction(struct yos_exec_ctx *ctx, int32_t signum,
                              uint32_t act, uint32_t oldact, uint32_t sigsetsize)
{
    (void)ctx;
    ydebug("rt_sigaction(sig=%d, act=0x%x, oldact=0x%x, size=%u) -> 0 (stub)\n",
           signum, act, oldact, sigsetsize);
    /* If oldact is provided, we should write the old action there.
     * For now, just zero it out if provided. */
    if (oldact && ctx) {
        /* Zero out the old sigaction struct (32 bytes on i386) */
        uint8_t *p = ctx->memory + oldact;
        if (oldact + 32 <= ctx->memory_size) {
            for (int i = 0; i < 32; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sig_rt_sigprocmask(struct yos_exec_ctx *ctx, int32_t how,
                                uint32_t set, uint32_t oset, uint32_t sigsetsize)
{
    (void)ctx; (void)how; (void)set; (void)sigsetsize;
    ydebug("rt_sigprocmask(how=%d) -> 0 (stub)\n", how);
    /* If oset is provided, zero it out */
    if (oset && ctx) {
        uint8_t *p = ctx->memory + oset;
        if (oset + 8 <= ctx->memory_size) {
            for (int i = 0; i < 8; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sigaction(struct yos_exec_ctx *ctx, int32_t signum,
                           uint32_t act, uint32_t oldact)
{
    (void)ctx;
    ydebug("sigaction(sig=%d) -> 0 (stub)\n", signum);
    if (oldact && ctx) {
        uint8_t *p = ctx->memory + oldact;
        if (oldact + 32 <= ctx->memory_size) {
            for (int i = 0; i < 32; i++) p[i] = 0;
        }
    }
    return 0;
}

int32_t yos_sigprocmask(struct yos_exec_ctx *ctx, int32_t how,
                             uint32_t set, uint32_t oset)
{
    (void)ctx; (void)how; (void)set;
    ydebug("sigprocmask(how=%d) -> 0 (stub)\n", how);
    if (oset && ctx) {
        uint8_t *p = ctx->memory + oset;
        if (oset + 8 <= ctx->memory_size) {
            for (int i = 0; i < 8; i++) p[i] = 0;
        }
    }
    return 0;
}
