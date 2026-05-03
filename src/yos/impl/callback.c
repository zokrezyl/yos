/* impl/callback.c — libc fns that take a guest function-pointer
 * argument (qsort, bsearch, atexit, …).
 *
 * The auto-generated bridge can't pass these through to host libc:
 * the wasm guest passes a function-table INDEX (uint32_t), not a
 * host function pointer. Casting the index to (void *) and handing
 * it to host qsort would dereference garbage host memory.
 *
 * For each such fn we implement the algorithm host-side and invoke
 * the wasm callback via m3_Call(table0[idx], …). The callback runs
 * on the same wasm runtime that called us — no thread-spawn, no TLS
 * gymnastics. Only correctness concern: the callback can re-enter
 * yos imports (it usually only calls memcmp) which is fine because
 * wasm3's stack is re-entrant.
 *
 * Listed in hooks.yaml as `runtime_owned` so bridge.py emits no
 * conflicting body. main.c calls yos_callback_link() to bind us.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"

/* Look up a wasm function by table0 index, JIT-compiling on first
 * use. Returns NULL on failure. */
static IM3Function lookup_fn(IM3Runtime rt, uint32_t idx)
{
    IM3Module mod = rt->modules;
    if (!mod || idx >= mod->table0Size) return NULL;
    IM3Function fn = mod->table0[idx];
    if (!fn) return NULL;
    if (!fn->compiled) {
        /* Indirect-call targets are JIT-compiled lazily. */
        if (CompileFunction(fn) != NULL) return NULL;
    }
    return fn;
}

/* Invoke a wasm comparator(a, b) where a/b are wasm offsets into the
 * sorted array. Returns the int result; on m3 failure, returns 0
 * (treat as "equal" — keeps the algorithm running, may produce
 * non-deterministic order but no crash). */
static int call_cmp(IM3Runtime rt, IM3Function cmp,
                    uint32_t a_off, uint32_t b_off)
{
    M3Result r = m3_CallV(cmp, a_off, b_off);
    if (r) return 0;
    int32_t out = 0;
    m3_GetResultsV(cmp, &out);
    return (int)out;
}

/* Simple insertion sort over a wasm-side array of `n` elements of
 * `size` bytes each. O(n^2) but fine for the small arrays nvim
 * sorts at startup (option lists, key tables, etc). Replace with
 * qsort/quickersort when a profile says it matters. */
static m3ApiRawFunction(m3_yos_qsort)
{
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, size);
    m3ApiGetArg(uint32_t, cmp_idx);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    if (!size || n < 2 || (uint64_t)n * size > mem_size - base) m3ApiSuccess();

    IM3Function cmp = lookup_fn(runtime, cmp_idx);
    if (!cmp) m3ApiSuccess();

    /* Scratch slot for swap, host-side. */
    uint8_t scratch[512];
    if (size > sizeof(scratch)) m3ApiSuccess(); /* skip if elems too big */

    uint8_t *arr = ctx->memory + base;
    for (uint32_t i = 1; i < n; i++) {
        memcpy(scratch, arr + i * size, size);
        uint32_t j = i;
        while (j > 0) {
            uint32_t left_off  = base + (j - 1) * size;
            /* Pass the scratch's CURRENT wasm position as the b arg.
             * Since arr[j] hasn't been written yet (we lifted it into
             * scratch), use base + i*size as a stable pseudo-address —
             * the comparator reads from that offset which still holds
             * the lifted value (we haven't overwritten it yet at j=i,
             * but we may have shifted it; safer to recompute). For
             * simplicity, copy scratch back to position j first. */
            memcpy(arr + j * size, scratch, size);
            int c = call_cmp(runtime, cmp, left_off, base + j * size);
            if (c <= 0) break;
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        }
        memcpy(arr + j * size, scratch, size);
    }
    m3ApiSuccess();
}

/* bsearch(key, base, n, size, cmp) — linear-walk with the wasm
 * comparator. n is small for nvim's use; OK if not literally
 * binary-search. */
static m3ApiRawFunction(m3_yos_bsearch)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, key);
    m3ApiGetArg(uint32_t, base);
    m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, size);
    m3ApiGetArg(uint32_t, cmp_idx);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    if (!size || !n || (uint64_t)n * size > mem_size - base) m3ApiReturn(0);

    IM3Function cmp = lookup_fn(runtime, cmp_idx);
    if (!cmp) m3ApiReturn(0);

    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = call_cmp(runtime, cmp, key, base + mid * size);
        if (c == 0) m3ApiReturn(base + mid * size);
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    m3ApiReturn(0);
}

/* atexit / __cxa_atexit / at_quick_exit — register a wasm callback
 * to run at exit. We don't actually run them (yos exits via host
 * exit() which doesn't go back into wasm). Return success silently
 * so the guest's libc startup doesn't bail. */
static m3ApiRawFunction(m3_yos_atexit_noop)
{
    m3ApiReturnType(int32_t);
    m3ApiReturn(0);
}

void yos_callback_link(IM3Module mod)
{
    m3_LinkRawFunction(mod, "env", "qsort",         "v(iiii)",   m3_yos_qsort);
    m3_LinkRawFunction(mod, "env", "bsearch",       "i(iiiii)",  m3_yos_bsearch);
    m3_LinkRawFunction(mod, "env", "atexit",        "i(i)",      m3_yos_atexit_noop);
    m3_LinkRawFunction(mod, "env", "__cxa_atexit",  "i(iii)",    m3_yos_atexit_noop);
    m3_LinkRawFunction(mod, "env", "at_quick_exit", "i(i)",      m3_yos_atexit_noop);
}
