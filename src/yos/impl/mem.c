#define _GNU_SOURCE
#include "yos/types.h"
#include <yos/ytrace/ytrace.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Forward declaration */
int32_t yos_mmap2(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t length,
                       int32_t prot, int32_t flags, int32_t fd, uint32_t pgoffset);

/* Find best-fit free region in ctx's free list. Returns 0 if none found.
 * Caller must hold ctx->mem_lock. */
static uint32_t find_free_region(struct yos_exec_ctx *ctx, uint32_t len)
{
    int best = -1;
    uint32_t best_size = UINT32_MAX;
    for (int i = 0; i < ctx->free_count; i++) {
        if (ctx->free_list[i].len >= len && ctx->free_list[i].len < best_size) {
            best = i;
            best_size = ctx->free_list[i].len;
        }
    }
    if (best < 0) return 0;

    uint32_t addr = ctx->free_list[best].addr;
    uint32_t remaining = ctx->free_list[best].len - len;

    if (remaining >= 4096) {
        /* Split: keep remainder in free list */
        ctx->free_list[best].addr += len;
        ctx->free_list[best].len = remaining;
    } else {
        /* Remove from free list */
        ctx->free_list[best] = ctx->free_list[--ctx->free_count];
    }
    return addr;
}

/* Add region to ctx's free list. Caller must hold ctx->mem_lock. */
static void add_free_region(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t len)
{
    if (ctx->free_count >= YOS_MAX_FREE_REGIONS) {
        /* List full, drop oldest */
        ctx->free_list[0] = ctx->free_list[--ctx->free_count];
    }
    ctx->free_list[ctx->free_count].addr = addr;
    ctx->free_list[ctx->free_count].len = len;
    ctx->free_count++;
}

int32_t yos_brk(struct yos_exec_ctx *ctx, uint32_t addr)
{
    if (addr == 0) {
        ydebug("brk(0) -> 0x%x (mem_size=0x%x)\n", ctx->heap_end, ctx->memory_size);
        return (int32_t)ctx->heap_end;
    }

    ydebug("brk(0x%x), heap_end=0x%x, mem_size=0x%x\n", addr, ctx->heap_end, ctx->memory_size);

    if (addr > ctx->memory_size) {
        ydebug("brk ENOMEM\n");
        return -ENOMEM;
    }

    /* Reserve the upper half of linear memory for anonymous mmap2 so
     * brk and mmap can never overlap. Real Linux keeps these in
     * separate VMAs; we can't (one flat wasm memory), so we keep them
     * disjoint by address. mmap2 anchors at memory_size/2 even before
     * its first call, so brk has to honor that boundary up front. */
    uint32_t mmap_anchor = ctx->mmap_top ? ctx->mmap_top : ctx->memory_size / 2;
    if (addr > mmap_anchor) {
        ydebug("brk ENOMEM (would cross mmap anchor=0x%x)\n", mmap_anchor);
        return -ENOMEM;
    }

    ctx->heap_end = addr;
    return (int32_t)addr;
}

/* mmap(addr, length, prot, flags, fd, offset) — POSIX. We support
 * anonymous mappings (fd == -1) by routing into yos_mmap2; non-
 * anonymous file mappings still return ENOSYS. The wasm-side
 * `pgoffset` stays in bytes; mmap2 ignores it for anonymous maps. */
int32_t yos_mmap(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t length,
                 int32_t prot, int32_t flags, int32_t fd, uint64_t offset)
{
    (void)offset;
    return yos_mmap2(ctx, addr, length, prot, flags, fd, 0);
}

int32_t yos_munmap(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t len)
{
    /* Align to page */
    len = (len + 4095) & ~4095;

    ydebug("munmap(0x%x, 0x%x) -> 0\n", addr, len);

    /* Zero out the memory region */
    if (addr + len <= ctx->memory_size) {
        memset(ctx->memory + addr, 0, len);
    }

    /* Add to free list for reuse */
    pthread_mutex_lock(&ctx->mem_lock);
    add_free_region(ctx, addr, len);
    pthread_mutex_unlock(&ctx->mem_lock);

    return 0;
}

/* mprotect / mlock / mlockall etc. on wasm linear memory:
 * wasm has one flat memory; per-region permission bits don't exist in
 * the wasm execution model and forwarding to host mprotect would
 * (mis)apply to the runtime's whole memory mapping. We accept the call,
 * sanity-check the range, and trace. Callers relying on hard W^X /
 * lock-in-RAM for security MUST NOT depend on these on wasm. */

static int32_t check_range(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len, const char *who)
{
    if (len == 0) return 0;
    if (start >= ctx->memory_size || len > ctx->memory_size ||
        start + len > ctx->memory_size) {
        ydebug("%s(0x%x, 0x%x) -> -ENOMEM (out of wasm memory 0x%x)\n",
               who, start, len, (uint32_t)ctx->memory_size);
        return -ENOMEM;
    }
    return 0;
}

int32_t yos_mprotect(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len, int32_t prot)
{
    ydebug("mprotect(0x%x, 0x%x, 0x%x) -> 0 (advisory; wasm has no per-region prot)\n",
           start, len, prot);
    return check_range(ctx, start, len, "mprotect");
}

int32_t yos_mlock(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len)
{
    ydebug("mlock(0x%x, 0x%x) -> 0 (advisory on wasm)\n", start, len);
    return check_range(ctx, start, len, "mlock");
}

int32_t yos_munlock(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len)
{
    ydebug("munlock(0x%x, 0x%x) -> 0 (advisory on wasm)\n", start, len);
    return check_range(ctx, start, len, "munlock");
}

int32_t yos_mlockall(struct yos_exec_ctx *ctx, int32_t flags)
{
    (void)ctx;
    ydebug("mlockall(0x%x) -> 0 (advisory on wasm)\n", flags);
    return 0;
}

int32_t yos_munlockall(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    ydebug("munlockall() -> 0 (advisory on wasm)\n");
    return 0;
}

int32_t yos_mremap(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t old_len, uint32_t new_len, int32_t flags, uint32_t new_addr)
{
    ydebug("mremap(0x%x, 0x%x, 0x%x, %d, 0x%x)\n", addr, old_len, new_len, flags, new_addr);
    (void)old_len; (void)flags; (void)new_addr;
    /* For wasm, we can't really remap memory. If growing, try to allocate new space */
    if (new_len <= old_len) {
        return addr; /* shrinking - just return same address */
    }
    /* For growing, allocate new space via mmap2 and copy */
    uint32_t new_addr_out = yos_mmap2(ctx, 0, new_len, 3 /*PROT_READ|PROT_WRITE*/,
                                           0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0);
    if ((int32_t)new_addr_out < 0) {
        return new_addr_out; /* error */
    }
    /* Copy old data to new location */
    memcpy(ctx->memory + new_addr_out, ctx->memory + addr, old_len);
    ydebug("mremap -> 0x%x\n", new_addr_out);
    return new_addr_out;
}

/* Simple mmap2 implementation - allocates from heap space
 * mmap2(addr, length, prot, flags, fd, pgoffset)
 * For anonymous mappings (fd=-1), just allocate heap space */
#define MAP_FIXED 0x10

int32_t yos_mmap2(struct yos_exec_ctx *ctx, uint32_t addr, uint32_t length,
                       int32_t prot, int32_t flags, int32_t fd, uint32_t pgoffset)
{
    (void)prot; (void)pgoffset;

    /* only support anonymous private mappings */
    if (fd != -1 && fd != 0xffffffff) {
        ydebug("mmap2: fd=%d not supported -> ENOSYS\n", fd);
        return -ENOSYS;
    }

    /* align length to page */
    length = (length + 4095) & ~4095;

    /* MAP_FIXED: use the requested address if it fits in memory */
    if ((flags & MAP_FIXED) && addr != 0) {
        if (addr + length > ctx->memory_size) {
            ydebug("mmap2: MAP_FIXED addr=0x%x len=0x%x -> ENOMEM\n", addr, length);
            return -ENOMEM;
        }
        /* anonymous mmap must return zeroed memory */
        memset(ctx->memory + addr, 0, length);
        ydebug("mmap2: MAP_FIXED addr=0x%x len=0x%x -> 0x%x\n", addr, length, addr);
        return (int32_t)addr;
    }

    pthread_mutex_lock(&ctx->mem_lock);

    /* Try to reuse a freed region first */
    uint32_t result = find_free_region(ctx, length);
    if (result) {
        pthread_mutex_unlock(&ctx->mem_lock);
        /* Reusing freed region - already zeroed by munmap */
        ydebug("mmap2: length=0x%x -> 0x%x (reused)\n", length, result);
        return (int32_t)result;
    }

    /* Anchor mmap2 well above the brk region so the heap can grow
     * without colliding. Without this, mmap2 returns chunks adjacent
     * to brk's heap, then a later brk grow stomps over them — silent
     * heap corruption when libuv/Lua read back garbage. */
    if (ctx->mmap_top == 0)
        ctx->mmap_top = ctx->memory_size / 2;
    if (ctx->mmap_top < ctx->heap_end) {
        ydebug("mmap2: brk grew past mmap anchor (heap_end=0x%x mmap_top=0x%x) -> ENOMEM\n",
               ctx->heap_end, ctx->mmap_top);
        pthread_mutex_unlock(&ctx->mem_lock);
        return -ENOMEM;
    }
    result = ctx->mmap_top;
    uint32_t new_end = result + length;

    if (new_end > ctx->memory_size) {
        pthread_mutex_unlock(&ctx->mem_lock);
        ydebug("mmap2: length=0x%x, mmap_top=0x%x -> ENOMEM\n", length, ctx->mmap_top);
        return -ENOMEM;
    }

    ctx->mmap_top = new_end;
    pthread_mutex_unlock(&ctx->mem_lock);

    /* anonymous mmap must return zeroed memory */
    memset(ctx->memory + result, 0, length);
    ydebug("mmap2: length=0x%x -> 0x%x\n", length, result);
    return (int32_t)result;
}

int32_t yos_madvise(struct yos_exec_ctx *ctx, uint32_t start, uint32_t len, int32_t behavior)
{
    ydebug("madvise(0x%x, 0x%x, %d) -> 0 (advisory)\n", start, len, behavior);
    return check_range(ctx, start, len, "madvise");
}
