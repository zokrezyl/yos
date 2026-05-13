/* alloc.c — yos guest-allocator backed by host mimalloc.
 *
 * Architecture:
 *   - mimalloc runs on the HOST, but its arena is a region of the
 *     GUEST's wasm linear memory. mi_malloc returns a host pointer
 *     INTO ctx->memory[]; we convert to wasm offset for the guest.
 *   - ONE shared arena per yos_exec_ctx (per fork) — that's the wasm
 *     linear-memory region we own. Within that arena, EACH host
 *     thread that allocates gets its own mi_heap_t. mimalloc's
 *     invariant is that a heap is single-threaded; nvim's libuv
 *     pthread workers call malloc from threads other than the one
 *     that first allocated, which used to trip a debug assertion
 *     (and, in release builds, silently corrupt mimalloc's
 *     free-lists). Per-thread heaps in the same arena fix that
 *     properly: cross-thread `free` already works (mimalloc uses an
 *     atomic enqueue to the owning thread's free list), so we only
 *     need to route allocations through THIS thread's heap.
 *   - Lazy init: claim the arena on first allocation; first thread
 *     in also creates the first heap. Subsequent threads create
 *     their own heap in the same arena via mi_heap_new_in_arena().
 *   - Bridges in yos_bridge.c (custom_alloc routing) call us with
 *     wasm-ABI args (uint32 offsets, uint32 sizes); we return uint32
 *     offsets. Errors -> 0 (NULL in wasm) for malloc-family,
 *     -ENOMEM for posix_memalign.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

#include "mimalloc.h"

#include "yos/types.h"
#include "yos/ydebug.h"
#include "alloc.h"

/* Initialisation guard. mimalloc itself is thread-safe; this lock
 * only serialises the one-shot arena/heap setup. */
static pthread_mutex_t alloc_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* Per-host-thread heap. Each thread that allocates for `ctx` gets
 * its own mi_heap_t into ctx's shared arena, satisfying mimalloc's
 * single-threaded-per-heap invariant. The pointer is keyed by the
 * arena id so a fork-spawned ctx's threads don't reuse a parent
 * ctx's heap. */
struct alloc_tls {
    int          arena_id;
    mi_heap_t   *heap;
};
static __thread struct alloc_tls alloc_tls;

/* Get (or create) THIS thread's heap into ctx's arena. */
static mi_heap_t *thread_heap(struct yos_exec_ctx *ctx)
{
    if (alloc_tls.heap && alloc_tls.arena_id == ctx->mi_arena_id)
        return alloc_tls.heap;
    mi_heap_t *h = mi_heap_new_in_arena((mi_arena_id_t)ctx->mi_arena_id);
    if (!h) return NULL;
    alloc_tls.heap     = h;
    alloc_tls.arena_id = ctx->mi_arena_id;
    return h;
}

/* Round x up to a multiple of 16 (mimalloc requires 16-aligned arena
 * starts on most platforms; matches our heap_end alignment too). */
static inline uint32_t align_up_16(uint32_t x) { return (x + 15u) & ~15u; }

/* Carve the heap region the first time we're called for this ctx.
 * Returns 0 on success, -1 if the layout is unworkable. */
static int alloc_init(struct yos_exec_ctx *ctx)
{
    if (ctx->mi_heap) return 0;

    pthread_mutex_lock(&alloc_init_lock);
    if (ctx->mi_heap) {
        pthread_mutex_unlock(&alloc_init_lock);
        return 0;
    }

    uint32_t lo = align_up_16(ctx->heap_end);
    /* Upper half is reserved for mmap2 (impl/mem.c brk() guards on
     * memory_size/2 too). The arena ends one page below that anchor
     * to leave a guard slack. */
    uint32_t hi = (ctx->memory_size / 2u) & ~0xfffu;
    if (hi <= lo + (1u << 20)) {
        /* Less than 1 MiB free for the heap — caller's memory is
         * mis-sized. Refuse; bridges will return NULL/-ENOMEM. */
        pthread_mutex_unlock(&alloc_init_lock);
        return -1;
    }
    uint32_t size = hi - lo;

    void *base = ctx->memory + lo;
    mi_arena_id_t aid = 0;
    bool ok = mi_manage_os_memory_ex(base, size,
                                     /*is_committed=*/true,
                                     /*is_large=*/false,
                                     /*is_zero=*/false,
                                     /*numa_node=*/-1,
                                     /*exclusive=*/true,
                                     &aid);
    if (!ok) {
        pthread_mutex_unlock(&alloc_init_lock);
        return -1;
    }

    mi_heap_t *heap = mi_heap_new_in_arena(aid);
    if (!heap) {
        pthread_mutex_unlock(&alloc_init_lock);
        return -1;
    }

    ctx->mi_arena_id = (int)aid;
    ctx->mi_heap     = heap;
    ctx->mi_arena_lo = lo;
    ctx->mi_arena_hi = hi;
    /* Push brk past the arena so a later brk() can't carve into our
     * region. impl/mem.c's brk() also guards on memory_size/2 but
     * being explicit avoids surprises if those bounds drift. */
    ctx->heap_end = hi;

    ydebug("alloc_init: arena=[0x%x..0x%x) (%u MiB), heap=%p\n",
           lo, hi, size >> 20, (void *)heap);

    pthread_mutex_unlock(&alloc_init_lock);
    return 0;
}

/* Convert a host pointer returned by mimalloc back to a wasm offset.
 * Returns 0 if p is NULL or sits outside ctx's linear memory (latter
 * is a programming error — mimalloc never lies about its arena —
 * but be defensive). */
static inline uint32_t host_to_wasm(struct yos_exec_ctx *ctx, void *p)
{
    if (!p) return 0;
    uintptr_t base = (uintptr_t)ctx->memory;
    uintptr_t hp   = (uintptr_t)p;
    if (hp < base) return 0;
    uintptr_t off  = hp - base;
    if (off >= ctx->memory_size) return 0;
    return (uint32_t)off;
}

/* Convert a wasm offset to host pointer for free/realloc inputs.
 * Returns NULL for off==0 (wasm NULL). */
static inline void *wasm_to_host(struct yos_exec_ctx *ctx, uint32_t off)
{
    if (off == 0) return NULL;
    if (off >= ctx->memory_size) return NULL;
    return ctx->memory + off;
}

uint32_t yos_malloc(struct yos_exec_ctx *ctx, uint32_t size)
{
    if (alloc_init(ctx) != 0) return 0;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return 0;
    void *p = mi_heap_malloc(h, size);
    return host_to_wasm(ctx, p);
}

void yos_free(struct yos_exec_ctx *ctx, uint32_t off)
{
    if (off == 0) return;
    /* mi_free assumes the pointer comes from a mimalloc allocation.
     * The wasm guest can hand us anything — including pointers from
     * its own internal allocators (libc startup sentinels, dlmalloc
     * fallbacks, …) or stale offsets after execve. Passing those to
     * mi_free walks page metadata that mimalloc never wrote and
     * trips the debug-build assert "heap!=NULL" in
     * mi_heap_page_queue_of. mi_check_owned() returns false for
     * non-mimalloc pointers; skip those silently — same behaviour
     * as free()ing a non-malloc pointer on a real system (UB, but
     * no crash). Init the arena first so the check can answer at
     * all. */
    if (alloc_init(ctx) != 0) return;
    if (off < ctx->mi_arena_lo || off >= ctx->mi_arena_hi) return;
    void *p = wasm_to_host(ctx, off);
    if (!p) return;
    if (!mi_check_owned(p)) return;
    mi_free(p);
}

uint32_t yos_calloc(struct yos_exec_ctx *ctx, uint32_t nmemb, uint32_t size)
{
    if (alloc_init(ctx) != 0) return 0;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return 0;
    void *p = mi_heap_calloc(h, nmemb, size);
    return host_to_wasm(ctx, p);
}

uint32_t yos_realloc(struct yos_exec_ctx *ctx, uint32_t off, uint32_t newsize)
{
    if (alloc_init(ctx) != 0) return 0;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return 0;
    void *p = NULL;
    if (off != 0) {
        if (off < ctx->mi_arena_lo || off >= ctx->mi_arena_hi)
            return 0;  /* Same as realloc-of-foreign-ptr: ignore. */
        p = wasm_to_host(ctx, off);
        if (p && !mi_check_owned(p)) return 0;
    }
    /* mi_heap_realloc(NULL) == mi_heap_malloc; mi_heap_realloc(p, 0)
     * frees p and returns NULL. Both match POSIX realloc. */
    void *q = mi_heap_realloc(h, p, newsize);
    return host_to_wasm(ctx, q);
}

uint32_t yos_reallocarray(struct yos_exec_ctx *ctx, uint32_t off,
                          uint32_t nmemb, uint32_t size)
{
    if (alloc_init(ctx) != 0) return 0;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return 0;
    /* Overflow check matches glibc/musl reallocarray semantics. */
    if (nmemb && size > UINT32_MAX / nmemb) return 0;
    void *p = NULL;
    if (off != 0) {
        if (off < ctx->mi_arena_lo || off >= ctx->mi_arena_hi) return 0;
        p = wasm_to_host(ctx, off);
        if (p && !mi_check_owned(p)) return 0;
    }
    void *q = mi_heap_realloc(h, p, (size_t)nmemb * size);
    return host_to_wasm(ctx, q);
}

int32_t yos_posix_memalign(struct yos_exec_ctx *ctx, uint32_t memptr_off,
                           uint32_t alignment, uint32_t size)
{
    if (alloc_init(ctx) != 0) return ENOMEM;
    if (memptr_off == 0 || memptr_off + 4 > ctx->memory_size) return EINVAL;
    /* POSIX: alignment must be power-of-two AND multiple of sizeof(void*).
     * On wasm32 sizeof(void*) == 4. */
    if (alignment < 4 || (alignment & (alignment - 1)) != 0) return EINVAL;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return ENOMEM;
    void *p = mi_heap_malloc_aligned(h, size, alignment);
    if (!p) return ENOMEM;
    uint32_t off = host_to_wasm(ctx, p);
    *(uint32_t *)(ctx->memory + memptr_off) = off;
    return 0;
}

uint32_t yos_aligned_alloc(struct yos_exec_ctx *ctx, uint32_t alignment,
                           uint32_t size)
{
    if (alloc_init(ctx) != 0) return 0;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;
    mi_heap_t *h = thread_heap(ctx);
    if (!h) return 0;
    void *p = mi_heap_malloc_aligned(h, size, alignment);
    return host_to_wasm(ctx, p);
}

uint32_t yos_memalign(struct yos_exec_ctx *ctx, uint32_t alignment,
                      uint32_t size)
{
    /* Same as aligned_alloc on glibc; aligned_alloc is the POSIX/C11
     * spelling. memalign is legacy but still used. */
    return yos_aligned_alloc(ctx, alignment, size);
}

uint32_t yos_valloc(struct yos_exec_ctx *ctx, uint32_t size)
{
    /* page-aligned; wasm pages are 64 KiB but mimalloc's notion of
     * page is the host's. 4 KiB matches musl/glibc's behaviour and is
     * what callers (rare) actually expect. */
    return yos_aligned_alloc(ctx, 4096, size);
}

uint32_t yos_malloc_usable_size(struct yos_exec_ctx *ctx, uint32_t off)
{
    void *p = wasm_to_host(ctx, off);
    if (!p) return 0;
    return (uint32_t)mi_usable_size(p);
}
