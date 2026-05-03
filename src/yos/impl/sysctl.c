/* impl/sysctl.c — minimal FreeBSD sysctl shim.
 *
 * The full FreeBSD sysctl namespace is huge and host-dependent. We
 * implement only the cases we know guests actually need; everything
 * else returns -ENOSYS. Today's set:
 *
 *   CTL_KERN.KERN_PROC.KERN_PROC_PATHNAME(-1)
 *     → path of the current "process". libuv's uv_exepath() uses
 *       this on FreeBSD-shaped builds. We return the wasm file
 *       path the host yos was invoked with (ctx->rt->argv[0]).
 *
 * Listed in hooks.yaml runtime_owned so bridge.py emits no body;
 * main.c calls yos_sysctl_link() to bind us.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"
#include "yos/ydebug.h"

/* FreeBSD sysctl MIB constants (from sys/sysctl.h, sys/proc.h). */
#define CTL_KERN              1
#define KERN_PROC            14
#define KERN_PROC_PATHNAME   12

static int32_t do_kern_proc_pathname(struct yos_exec_ctx *ctx,
                                     uint32_t old_off, uint32_t oldlenp_off)
{
    if (!ctx->rt || !ctx->rt->argv || !ctx->rt->argv[0]) return -ENOENT;
    const char *path = ctx->rt->argv[0];
    size_t need = strlen(path) + 1;

    /* Read caller's old buffer length (oldlenp is a uint32_t* in
     * wasm32). NULL oldlenp => caller wants the size only via the
     * call's normal return path; FreeBSD documents ENOMEM in that
     * scenario. We treat NULL as "no buffer" too. */
    uint32_t buflen = 0;
    if (oldlenp_off) {
        if (oldlenp_off + 4 > ctx->memory_size) return -EFAULT;
        buflen = *(uint32_t *)(ctx->memory + oldlenp_off);
    }

    if (old_off == 0) {
        /* Caller is asking for the size only. */
        if (oldlenp_off)
            *(uint32_t *)(ctx->memory + oldlenp_off) = (uint32_t)need;
        return 0;
    }

    if (buflen < need) {
        if (oldlenp_off)
            *(uint32_t *)(ctx->memory + oldlenp_off) = (uint32_t)need;
        return -ENOMEM;
    }
    if (old_off + need > ctx->memory_size) return -EFAULT;
    memcpy(ctx->memory + old_off, path, need);
    if (oldlenp_off)
        *(uint32_t *)(ctx->memory + oldlenp_off) = (uint32_t)need;
    return 0;
}

/* sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
 *        const void *newp, size_t newlen) */
static m3ApiRawFunction(m3_yos_sysctl)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_off);
    m3ApiGetArg(uint32_t, namelen);
    m3ApiGetArg(uint32_t, oldp_off);
    m3ApiGetArg(uint32_t, oldlenp_off);
    m3ApiGetArg(uint32_t, newp_off);
    m3ApiGetArg(uint32_t, newlen);
    (void)newp_off; (void)newlen;

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    if (namelen < 2 || name_off + 4u * namelen > mem_size) m3ApiReturn(-EINVAL);
    int32_t *mib = (int32_t *)(ctx->memory + name_off);

    if (mib[0] == CTL_KERN && mib[1] == KERN_PROC && namelen >= 3 &&
        mib[2] == KERN_PROC_PATHNAME) {
        ydebug("sysctl(KERN_PROC_PATHNAME)\n");
        m3ApiReturn(do_kern_proc_pathname(ctx, oldp_off, oldlenp_off));
    }

    ydebug("sysctl: unhandled mib[%u] = {%d, %d, %d, ...}\n",
           namelen, mib[0], namelen > 1 ? mib[1] : 0,
           namelen > 2 ? mib[2] : 0);
    m3ApiReturn(-ENOSYS);
}

/* sysctlbyname(const char *name, …) — same idea, name lookup form. */
static m3ApiRawFunction(m3_yos_sysctlbyname)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_off);
    m3ApiGetArg(uint32_t, oldp_off);
    m3ApiGetArg(uint32_t, oldlenp_off);
    m3ApiGetArg(uint32_t, newp_off);
    m3ApiGetArg(uint32_t, newlen);
    (void)oldp_off; (void)oldlenp_off; (void)newp_off; (void)newlen;

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    const char *name = (name_off && name_off < mem_size)
                     ? (const char *)(ctx->memory + name_off) : "?";
    ydebug("sysctlbyname(\"%s\") -> ENOSYS\n", name);
    m3ApiReturn(-ENOSYS);
}

void yos_sysctl_link(IM3Module mod)
{
    m3_LinkRawFunction(mod, "env", "sysctl",       "i(iiiiii)", m3_yos_sysctl);
    m3_LinkRawFunction(mod, "env", "sysctlbyname", "i(iiiii)",  m3_yos_sysctlbyname);
}
