#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"
#include "yos/ydebug.h"
#include "yos/vfs/mount.h"
#include "yos/vfs/procfs.h"
#include "impl/pthread.h"
#include "impl/tier2.h"

/* Legacy yos-private number-indexed dispatcher REMOVED — see CLAUDE.md
 * non-negotiable #5. The wasm guest must import each libc function by
 * name (env.read, env.write, …) and `yos_brg_link_imports()` from
 * the auto-generated bridges resolves them. */

/* Variadic libc family — printf/fprintf/sprintf/snprintf and the v*
 * variants. Wasm has no `...` so clang lowers each call to a non-
 * variadic call (fmt_off, va_list_off). impl/printf.c walks fmt and
 * reads va_list from guest memory. The m3w_<name> trampolines below
 * only differ in argument unpacking; all delegate to one of the
 * yos_*printf functions. */
extern int32_t yos_printf  (struct yos_exec_ctx *ctx,
                            uint32_t fmt_off, uint32_t va_off);
extern int32_t yos_fprintf (struct yos_exec_ctx *ctx,
                            uint32_t fp, uint32_t fmt_off, uint32_t va_off);
extern int32_t yos_sprintf (struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t fmt, uint32_t va);
extern int32_t yos_snprintf(struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t n, uint32_t fmt, uint32_t va);
extern int32_t yos_vprintf (struct yos_exec_ctx *ctx,
                            uint32_t fmt, uint32_t va);
extern int32_t yos_vfprintf(struct yos_exec_ctx *ctx,
                            uint32_t fp, uint32_t fmt, uint32_t va);
extern int32_t yos_vsprintf(struct yos_exec_ctx *ctx,
                            uint32_t dst, uint32_t fmt, uint32_t va);
extern int32_t yos_vsnprintf(struct yos_exec_ctx *ctx,
                             uint32_t dst, uint32_t n, uint32_t fmt, uint32_t va);

/* Helper: refresh ctx->memory for the wrappers below. */
static inline void pfx_refresh(IM3Runtime rt, struct yos_exec_ctx *ctx) {
    uint32_t ms = 0;
    ctx->memory = m3_GetMemory(rt, &ms, 0);
    ctx->memory_size = ms;
}

static m3ApiRawFunction(m3_printf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_printf(ctx, fmt, va));
}
static m3ApiRawFunction(m3_fprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fp); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_fprintf(ctx, fp, fmt, va));
}
static m3ApiRawFunction(m3_sprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_sprintf(ctx, dst, fmt, va));
}
static m3ApiRawFunction(m3_snprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_snprintf(ctx, dst, n, fmt, va));
}
static m3ApiRawFunction(m3_vprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_vprintf(ctx, fmt, va));
}
static m3ApiRawFunction(m3_vfprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, fp); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_vfprintf(ctx, fp, fmt, va));
}
static m3ApiRawFunction(m3_vsprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, fmt);
    m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_vsprintf(ctx, dst, fmt, va));
}
static m3ApiRawFunction(m3_vsnprintf) {
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, dst); m3ApiGetArg(uint32_t, n);
    m3ApiGetArg(uint32_t, fmt); m3ApiGetArg(uint32_t, va);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    pfx_refresh(runtime, ctx);
    m3ApiReturn(yos_vsnprintf(ctx, dst, n, fmt, va));
}

/* env.__main_argc_argv: clang's wasm32 command-exec-model renames a
 * user-defined `int main(int, char**)` to `__main_argc_argv` and emits
 * a wrapper `int main(void)` that's exported. Our crt1's call to
 * `main(argc, argv)` therefore lowers to env.__main_argc_argv. We
 * resolve it host-side to the wasm module's *real* main implementation
 * — which is exported under both names depending on linker behaviour
 * but reachable as func "main" via m3_FindFunction. The lookup is
 * lazy on first call (the module is fully loaded by then). */
static m3ApiRawFunction(m3_main_argc_argv)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, argc);
    m3ApiGetArg(int32_t, argv);

    /* Diagnostic: print what argv nvim is actually receiving. */
    {
        struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
        uint32_t ms = 0;
        ctx->memory = m3_GetMemory(runtime, &ms, 0);
        ctx->memory_size = ms;
        fprintf(stderr, "yos: main(argc=%d, argv=0x%x)\n", argc, argv);
        uint32_t *av = (uint32_t *)(ctx->memory + (uint32_t)argv);
        for (int i = 0; i < argc && i < 4; i++) {
            uint32_t s_off = av[i];
            const char *s = (s_off && s_off < ms)
                          ? (const char *)(ctx->memory + s_off)
                          : "<bad>";
            fprintf(stderr, "yos:   argv[%d] off=0x%x \"%s\"\n", i, s_off, s);
        }
    }

    static IM3Function f_main;
    if (!f_main) {
        M3Result r = m3_FindFunction(&f_main, runtime, "__main_argc_argv");
        if (r || !f_main) r = m3_FindFunction(&f_main, runtime, "main");
        if (r || !f_main) {
            fprintf(stderr, "yos: __main_argc_argv: no main in module\n");
            m3ApiReturn(-1);
        }
    }
    M3Result r = m3_CallV(f_main, argc, argv);
    if (r) {
        fprintf(stderr, "yos: main trapped: %s\n", r);
        m3ApiReturn(-1);
    }
    int32_t rc = 0;
    m3_GetResultsV(f_main, &rc);
    m3ApiReturn(rc);
}

/* Tier-2 demo: forwards env.__yos_t2_demo(int, int) into the sidecar
 * wasm runtime that hosts libc-pure.wasm. Once bridge.py can emit
 * `from_freebsd_src` wrappers programmatically, this hand-written
 * helper goes away. Kept here as the canary for the sidecar
 * dispatch path. */
static m3ApiRawFunction(m3_t2_demo)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, a);
    m3ApiGetArg(int32_t, b);
    (void)runtime; (void)_ctx; (void)_mem;

    static IM3Function f_demo;
    IM3Function f = yos_tier2_resolve_once(&f_demo, "__yos_t2_demo");
    if (!f) m3ApiReturn((int32_t)-38 /* -ENOSYS */);

    M3Result r = m3_CallV(f, a, b);
    if (r) {
        fprintf(stderr, "yos: tier2 __yos_t2_demo: %s\n", r);
        m3ApiReturn((int32_t)-1);
    }
    int32_t out = 0;
    m3_GetResultsV(f, &out);
    m3ApiReturn(out);
}

/* stub for unresolved imports */
static m3ApiRawFunction(m3_unresolved_stub)
{
    IM3Function fn = _ctx->function;
    fprintf(stderr, "yos: unresolved import %s.%s\n",
            fn->import.moduleUtf8, fn->import.fieldUtf8);
    m3ApiTrap("unresolved import");
}

/* ----------------------------------------------------------------------------
 * setjmp / longjmp via asyncify
 *
 * Same trick fork uses. setjmp's first call kicks off an asyncify unwind so
 * the call stack state is captured into a private buffer (sj_asyncify_ptr).
 * The pump in the main exec loop sees `setjmp_pending`, stops the unwind,
 * starts a rewind from the same buffer, and re-calls _start. The rewind
 * walks back down to setjmp, which detects ASYNCIFY_REWINDING and returns
 * (with value 0 — first-time semantics).
 *
 * longjmp does the analogous unwind into a discard buffer and sets
 * `longjmp_pending` + `longjmp_value`. The pump rewinds from the SAVED
 * setjmp buffer (sj_asyncify_ptr), and m3_setjmp's REWINDING path returns
 * `longjmp_value` to the original setjmp caller.
 *
 * Limitations: only the most recent live setjmp is supported (one buffer per
 * ctx). Nested setjmps would need a stack of buffers; ash and our libc-
 * internal users only set up one at a time.
 * --------------------------------------------------------------------------*/

/* Asyncify save area for setjmp/longjmp. Bigger than fork's because the
 * captured stack at setjmp/longjmp time can include musl/libc frames that
 * don't appear in the simpler fork() path. */
#define SJ_BUF_SIZE  (64 * 1024)

static void main_call_asyncify(IM3Runtime rt, const char *name, uint32_t arg)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, name) == NULL) {
        if (arg != (uint32_t)-1) m3_CallV(f, arg);
        else                     m3_CallV(f);
    }
}

static int main_get_asyncify_state(IM3Runtime rt)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, "asyncify_get_state")) return -1;
    m3_CallV(f);
    int32_t state;
    m3_GetResultsV(f, &state);
    return state;
}

static void main_init_asyncify_buf(struct yos_exec_ctx *ctx, uint32_t buf_ptr)
{
    uint32_t *buf = (uint32_t *)(ctx->memory + buf_ptr);
    buf[0] = buf_ptr + 8;
    buf[1] = buf_ptr + SJ_BUF_SIZE;
}

/* Forward decls — definitions follow m3_setjmp/m3_longjmp. */
static int  sj_find_slot (struct yos_exec_ctx *ctx, uint32_t key);
static int  sj_alloc_slot(struct yos_exec_ctx *ctx, uint32_t jmp_buf_ptr,
                          uint32_t mem_size);
static void sj_reap_dead (struct yos_exec_ctx *ctx);

m3ApiRawFunction(m3_setjmp)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, jmp_buf_ptr);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    /* refresh memory pointer in case it grew since last call */
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    /* Rewind path. Three cases:
     *
     * (1) Longjmp pending, OUR jmp_buf is its target — actual landing.
     *     Stop the rewind, return val, free the slot.
     *
     * (2) Longjmp pending, NOT for us — we're an outer frame on the
     *     rewind path with a setjmp call that's on the way to the
     *     matching setjmp. Return 0 (the original first-call value) so
     *     the replay continues past us. Do NOT start a new unwind.
     *     (Earlier version that did is what spun test_setjmp #4 forever.)
     *
     * (3) No longjmp pending, asyncify REWINDING — first-call
     *     round-trip from yos_setjmp_pump for THIS slot. Stop, return 0.
     *     Slot stays live until matched longjmp / scope exit. */
    int astate = main_get_asyncify_state(runtime);
    if (ctx->longjmp_pending) {
        if (ctx->longjmp_target == jmp_buf_ptr) {
            int32_t val = ctx->longjmp_value;
            ctx->longjmp_pending = 0;
            ctx->longjmp_target  = 0;
            if (astate == ASYNCIFY_REWINDING)
                main_call_asyncify(runtime, "asyncify_stop_rewind", -1);
            /* Do NOT free the slot. C semantics: a setjmp may be the
             * target of an unbounded number of longjmps as long as its
             * owning function hasn't returned. Lua's pcall, ash's
             * commandloop both rely on this. The slot is recycled when
             * the same jmp_buf address gets a fresh setjmp call (treated
             * as "first call" again because longjmp_pending is clear). */
            ydebug("setjmp(%u): post-longjmp landing, returning %d\n",
                   jmp_buf_ptr, val);
            m3ApiReturn(val);
        }
        ydebug("setjmp(%u): replay pass-through (longjmp target=%u)\n",
               jmp_buf_ptr, ctx->longjmp_target);
        m3ApiReturn(0);
    }
    if (astate == ASYNCIFY_REWINDING) {
        main_call_asyncify(runtime, "asyncify_stop_rewind", -1);
        ydebug("setjmp(%u): first-call rewind complete, returning 0\n",
               jmp_buf_ptr);
        m3ApiReturn(0);
    }

    /* First call. Allocate (or reuse) a slot — one asyncify buffer per
     * live setjmp so an inner setjmp can't trample the outer's saved
     * state. Kick off the unwind into THIS slot's buffer. */
    int slot = sj_find_slot(ctx, jmp_buf_ptr);
    if (slot < 0) slot = sj_alloc_slot(ctx, jmp_buf_ptr, mem_size);
    if (slot < 0) {
        /* Try harvesting slots whose owning frame has unwound off the
         * shadow stack. Only kicks in once the table is full so the
         * common case stays cheap. */
        sj_reap_dead(ctx);
        slot = sj_alloc_slot(ctx, jmp_buf_ptr, mem_size);
    }
    if (slot < 0) {
        fprintf(stderr,
                "yos: setjmp slot table exhausted (%zu live setjmps)\n",
                sizeof(ctx->sj_slots)/sizeof(ctx->sj_slots[0]));
        m3ApiReturn(-1);
    }
    main_init_asyncify_buf(ctx, ctx->sj_slots[slot].asyncify_buf);

    ctx->setjmp_pending      = 1;
    ctx->setjmp_pending_slot = slot;
    ydebug("setjmp(%u): starting unwind, slot=%d buf=%u\n",
           jmp_buf_ptr, slot, ctx->sj_slots[slot].asyncify_buf);
    main_call_asyncify(runtime, "asyncify_start_unwind",
                       ctx->sj_slots[slot].asyncify_buf);

    m3ApiReturn(0);  /* placeholder; the real return comes after the rewind */
}

/* Slot allocator + lookup helpers. Defined out-of-line so the unit-test
 * build of yos can be inspected without macros. */
static int sj_find_slot(struct yos_exec_ctx *ctx, uint32_t key)
{
    if (key == 0) return -1;  /* 0 means "unused slot"; never legitimately match */
    for (size_t i = 0; i < sizeof(ctx->sj_slots)/sizeof(ctx->sj_slots[0]); i++)
        if (ctx->sj_slots[i].jmp_buf_ptr == key) return (int)i;
    return -1;
}

/* Read the wasm shadow stack pointer. Returns 0 if the global isn't
 * exported; callers must treat 0 as "unknown — don't recycle".  */
static uint32_t sj_get_stack_pointer(struct yos_exec_ctx *ctx)
{
    IM3Module mod = (IM3Module)ctx->module;
    if (!mod) return 0;
    IM3Global g = m3_FindGlobal(mod, "__stack_pointer");
    if (!g) return 0;
    M3TaggedValue tv = { 0 };
    if (m3_GetGlobal(g, &tv) || tv.type != c_m3Type_i32) return 0;
    return (uint32_t)tv.value.i32;
}

/* Reclaim slots whose jmp_buf lives in the popped portion of the wasm
 * shadow stack. wasm32 musl's stack grows DOWN, so a jmp_buf at addr X
 * is alive only while SP ≤ X. Once the owning function returns the
 * stack pointer climbs back above X and the jmp_buf address is dead;
 * a future longjmp targeting it would be UB anyway (C18 §7.13.2.1¶2).
 * Without this sweep, Lua's pcall pattern (each pcall registers a
 * fresh sigsetjmp at a deeper stack frame) exhausts the slot table
 * after a few hundred pcalls. */
static void sj_reap_dead(struct yos_exec_ctx *ctx)
{
    uint32_t sp = sj_get_stack_pointer(ctx);
    if (!sp) return;
    const size_t N = sizeof(ctx->sj_slots) / sizeof(ctx->sj_slots[0]);
    for (size_t i = 0; i < N; i++) {
        uint32_t jb = ctx->sj_slots[i].jmp_buf_ptr;
        if (jb == 0) continue;
        if (jb < sp) {
            /* Owning frame has returned — slot is dead. */
            ctx->sj_slots[i].jmp_buf_ptr = 0;
            free(ctx->sj_slots[i].save_data);
            ctx->sj_slots[i].save_data = NULL;
            ctx->sj_slots[i].save_size = 0;
        }
    }
}

static int sj_alloc_slot(struct yos_exec_ctx *ctx, uint32_t jmp_buf_ptr,
                          uint32_t mem_size)
{
    const size_t N = sizeof(ctx->sj_slots) / sizeof(ctx->sj_slots[0]);
    for (size_t i = 0; i < N; i++) {
        if (ctx->sj_slots[i].jmp_buf_ptr == 0) {
            ctx->sj_slots[i].jmp_buf_ptr = jmp_buf_ptr;
            /* Layout above the discard buffer (mem_size - 3*SJ_BUF_SIZE),
             * one buffer per slot moving DOWN. */
            ctx->sj_slots[i].asyncify_buf =
                mem_size - (uint32_t)((4 + i) * SJ_BUF_SIZE);
            return (int)i;
        }
    }
    return -1;
}

m3ApiRawFunction(m3_longjmp)
{
    m3ApiGetArg(uint32_t, jmp_buf_ptr);
    m3ApiGetArg(int32_t,  val);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    /* Reject longjmps to a buffer no setjmp has registered. */
    int slot = sj_find_slot(ctx, jmp_buf_ptr);
    if (slot < 0) {
        ydebug("longjmp(%u) without matching setjmp, exiting %d\n",
               jmp_buf_ptr, val);
        exit(val ? val : 1);
    }

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    if (ctx->sj_discard_ptr == 0)
        ctx->sj_discard_ptr = mem_size - 3 * SJ_BUF_SIZE;
    main_init_asyncify_buf(ctx, ctx->sj_discard_ptr);

    ctx->longjmp_pending = 1;
    /* C standard: longjmp(jb, 0) -> setjmp returns 1. Callers must be
     * able to distinguish first-call (0) from landing. */
    ctx->longjmp_value  = (val == 0) ? 1 : val;
    ctx->longjmp_target = jmp_buf_ptr;
    ydebug("longjmp(%u, %d): unwinding to discard=%u\n",
           jmp_buf_ptr, val, ctx->sj_discard_ptr);
    main_call_asyncify(runtime, "asyncify_start_unwind", ctx->sj_discard_ptr);

    m3ApiSuccess();
}

/* Pump for setjmp/longjmp. Drives the asyncify unwind→rewind round-trip.
 *
 * Asyncify's rewind CONSUMES the save buffer as it replays frames, so we
 * keep a host-side snapshot per slot. setjmp_pending means "a slot's
 * first-call unwind is in flight: snapshot its buffer, then rewind".
 * longjmp_pending means "kick off rewind from the matching slot's
 * (host-side) snapshot" — the current wasm-side buffer is from longjmp's
 * own unwind into sj_discard_ptr and is discarded.
 *
 * Loops while another setjmp/longjmp fires during the rewound run
 * (ash registers a fresh sigsetjmp every command-loop iteration).
 *
 * Non-static so the fork-thread exec loop in yos-proc.c can drive the
 * child runtime's setjmp/longjmp too — without that, the child's first
 * setjmp unwinds out of _start and the runtime treats it as a clean
 * exit. */
void yos_setjmp_pump(struct yos_exec_ctx *ctx)
{
    IM3Runtime rt = (IM3Runtime)ctx->runtime;
    while (ctx->setjmp_pending || ctx->longjmp_pending) {
        main_call_asyncify(rt, "asyncify_stop_unwind", -1);

        uint32_t mem_size = 0;
        ctx->memory = m3_GetMemory(rt, &mem_size, 0);
        ctx->memory_size = mem_size;

        int slot;
        if (ctx->setjmp_pending) {
            slot = ctx->setjmp_pending_slot;
            /* setjmp's unwind freshly populated the slot's buffer.
             * Snapshot it so future longjmps can replay. Detect
             * overflow up front — quieter than the OOB trap that the
             * rewind would otherwise produce when its bookkeeping
             * pointer points past the buffer. */
            struct yos_sj_slot *s = &ctx->sj_slots[slot];
            uint32_t *hdr = (uint32_t *)(ctx->memory + s->asyncify_buf);
            uint32_t used = hdr[0] - (s->asyncify_buf + 8);
            uint32_t cap  = hdr[1] - (s->asyncify_buf + 8);
            if (used > cap) {
                fprintf(stderr,
                        "yos: asyncify save overflowed slot=%d used=%u cap=%u\n",
                        slot, used, cap);
            }
            if (!s->save_data) {
                s->save_data = malloc(SJ_BUF_SIZE);
                s->save_size = SJ_BUF_SIZE;
            }
            if (s->save_data)
                memcpy(s->save_data, ctx->memory + s->asyncify_buf, SJ_BUF_SIZE);
        } else {
            /* longjmp landed on the pump. Rewind from the TARGET slot's
             * host-side snapshot back into its wasm buffer (asyncify's
             * own start_rewind will consume the wasm buffer as it
             * replays). */
            slot = sj_find_slot(ctx, ctx->longjmp_target);
            if (slot < 0) {
                fprintf(stderr,
                        "yos: pump cannot find slot for longjmp target %u\n",
                        ctx->longjmp_target);
                ctx->longjmp_pending = 0;
                ctx->longjmp_target  = 0;
                return;
            }
            struct yos_sj_slot *s = &ctx->sj_slots[slot];
            if (s->save_data)
                memcpy(ctx->memory + s->asyncify_buf, s->save_data, SJ_BUF_SIZE);
        }

        main_call_asyncify(rt, "asyncify_start_rewind",
                           ctx->sj_slots[slot].asyncify_buf);
        ctx->setjmp_pending      = 0;
        ctx->setjmp_pending_slot = -1;
        /* longjmp_pending is cleared by m3_setjmp's matching landing. */

        IM3Function start;
        if (m3_FindFunction(&start, rt, "_start") == NULL) {
            M3Result pr = m3_CallV(start);
            if (pr) {
                /* Inner _start trapped — propagate by clearing pending
                 * flags and stashing the trap so the caller sees it.
                 * Without this, a wasm crash inside the rewound _start
                 * would silently fall through as if the pump finished
                 * cleanly. */
                ctx->setjmp_pending = 0;
                ctx->longjmp_pending = 0;
                ctx->pump_trap = pr;
                return;
            }
        }
    }
}

/* m3_syscall_cp_asm + m3_yos_syscall removed — see CLAUDE.md non-
 * negotiable #5. Memory-pointer refresh after `memory.grow` now happens
 * in each generated bridge wrapper instead (TODO: extend bridge.py to
 * emit the refresh; for now bridges run after the initial GetMemory
 * snapshot in main()). */

/* __yos_argc() - return command line argument count */
m3ApiRawFunction(m3_yos_argc)
{
    m3ApiReturnType(int32_t);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    ydebug("m3_yos_argc: ctx=%p, runtime=%p\n", (void*)ctx, (void*)runtime);
    if (ctx) {
        ydebug("m3_yos_argc: ctx->argc=%d, ctx->argv=%p\n", ctx->argc, (void*)ctx->argv);
    } else {
        ydebug("m3_yos_argc: ctx is NULL!\n");
    }
    m3ApiReturn(ctx->argc);
}

/* __yos_argv_setup(char **argv) - copy argv strings to wasm memory */
m3ApiRawFunction(m3_yos_argv_setup)
{
    m3ApiGetArg(uint32_t, argv_ptr);  /* wasm pointer to char*[] */

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    ydebug("argv_setup: argc=%d, argv[0]=%s\n", ctx->argc, ctx->argv[0]);

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    fprintf(stderr,
            "yos: argv_setup: argv_ptr=0x%x mem_size=0x%x heap_end=0x%x\n",
            argv_ptr, mem_size, ctx->heap_end);
    if (argv_ptr + 4u * (uint32_t)(ctx->argc + 1) > mem_size) {
        fprintf(stderr,
                "yos: argv_setup: argv_ptr out of range; trapping\n");
        m3ApiTrap("argv_setup: bad argv_ptr");
    }

    /* allocate space for strings at end of current heap. heap_end is
     * normally `__heap_base` from the wasm-ld output; if it's still
     * zero (we never wrote it after load), park strings RIGHT AFTER
     * the argv array — the guest only reads strings via the
     * pointers we write, so they don't have to live at the
     * canonical heap location. */
    uint32_t str_ptr = ctx->heap_end;
    if (str_ptr == 0) {
        str_ptr = (argv_ptr + 4u * ((uint32_t)ctx->argc + 1) + 15u) & ~15u;
    }

    uint32_t *argv_arr = (uint32_t *)(ctx->memory + argv_ptr);

    for (int i = 0; i < ctx->argc; i++) {
        size_t len = strlen(ctx->argv[i]) + 1;
        if (str_ptr + len > ctx->memory_size) {
            fprintf(stderr, "yos: out of memory for argv\n");
            m3ApiTrap("out of memory");
        }
        memcpy(ctx->memory + str_ptr, ctx->argv[i], len);
        argv_arr[i] = str_ptr;
        str_ptr += len;
    }
    argv_arr[ctx->argc] = 0;  /* NULL terminate */

    ctx->heap_end = (str_ptr + 15) & ~15;  /* align to 16 */

    m3ApiSuccess();
}

/* __yos_envc() — number of environment variables to pass to wasm.
 * crt1 calls this before __yos_envp_setup so it can size the env array. */
m3ApiRawFunction(m3_yos_envc)
{
    m3ApiReturnType(int32_t);
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    int n = ctx ? ctx->envc : 0;
    ydebug("envc -> %d\n", n);
    m3ApiReturn(n);
}

/* __yos_envp_setup(char **envp) — copy environment strings into wasm
 * memory and fill the wasm-side envp array (NULL-terminated). Mirrors
 * argv_setup's bump allocator. Without this, getenv("PATH") in the
 * wasm always returns NULL and shells fall back to a baked-in default
 * PATH that doesn't match what the user asked for on the host. */
m3ApiRawFunction(m3_yos_envp_setup)
{
    m3ApiGetArg(uint32_t, envp_ptr);

    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);

    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(runtime, &mem_size, 0);
    ctx->memory_size = mem_size;

    uint32_t str_ptr = ctx->heap_end;
    uint32_t *envp_arr = (uint32_t *)(ctx->memory + envp_ptr);

    int n = ctx->envc;
    ydebug("envp_setup: envc=%d heap_end=0x%x\n", n, ctx->heap_end);
    for (int i = 0; i < n; i++) {
        const char *s = ctx->envp[i];
        size_t len = strlen(s) + 1;
        if (str_ptr + len > ctx->memory_size) {
            fprintf(stderr, "yos: out of memory for envp\n");
            m3ApiTrap("out of memory");
        }
        memcpy(ctx->memory + str_ptr, s, len);
        envp_arr[i] = str_ptr;
        str_ptr += len;
    }
    envp_arr[n] = 0;
    ydebug("envp_setup done, heap_end now 0x%x\n", (str_ptr + 15) & ~15);

    ctx->heap_end = (str_ptr + 15) & ~15;
    m3ApiSuccess();
}

/* Link all YOS imports to a module - used by both main and fork child */
void yos_link_imports(IM3Module module, struct yos_exec_ctx *ctx)
{
    /* env.__yos_syscall + env.__syscall_cp_asm bindings removed —
     * see CLAUDE.md non-negotiable #5. The new convention: each libc
     * function is its own env import, resolved by yos_brg_link_imports
     * below. */
    m3_LinkRawFunctionEx(module, "env", "__yos_argc",
                         "i()", m3_yos_argc, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_argv_setup",
                         "v(i)", m3_yos_argv_setup, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_envc",
                         "i()", m3_yos_envc, ctx);
    m3_LinkRawFunctionEx(module, "env", "__yos_envp_setup",
                         "v(i)", m3_yos_envp_setup, ctx);
    m3_LinkRawFunction(module, "env", "setjmp", "i(i)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "longjmp", "v(ii)", m3_longjmp);
    m3_LinkRawFunction(module, "env", "_setjmp", "i(i)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "_longjmp", "v(ii)", m3_longjmp);
    m3_LinkRawFunction(module, "env", "sigsetjmp", "i(ii)", m3_setjmp);
    m3_LinkRawFunction(module, "env", "siglongjmp", "v(ii)", m3_longjmp);

    /* L1 pthread imports — bind every yos_pthread_* the musl wasm32 shim
     * (src/musl/arch/wasm32/thread/yos_pthread_shim.c) calls into.  Lazy-
     * allocate the host on first use; main/proc subsystems share it via
     * g_runtime->pthread_host. */
    if (ctx && ctx->rt) {
        struct yos_pthread_host *ph = (struct yos_pthread_host *)ctx->rt->pthread_host;
        if (!ph) {
            /* Pass the master runtime + the wasm bytes so worker_main
             * can m3_NewSiblingRuntime + re-parse the same module on
             * each thread. Without these, clone() spawns a thread
             * that immediately fails to ParseModule(NULL) and bails
             * silently. The wasm-bytes pointer must outlive every
             * thread; ctx->wasm_bytes is kept around for fork too. */
            IM3Runtime master = (IM3Runtime)ctx->runtime;
            IM3Environment env = master ? master->environment : NULL;
            ph = yos_pthread_host_create (env, master,
                                          ctx->wasm_bytes,
                                          (uint32_t)ctx->wasm_bytes_size,
                                          /*per_thread_stack=*/64 * 1024,
                                          /*tls_pool_base=*/0,
                                          /*tls_arena_size=*/0);
            ctx->rt->pthread_host = ph;
        }
        if (ph)
            (void)yos_pthread_host_link (ph, module);
    }

    /* Soft-f128 builtins. clang's wasm32 ABI makes `long double` =
     * binary128, and musl's printf/strtod path uses long double freely.
     * We implement the compiler-rt-style __*tf* helpers in the host. */
    extern void yos_f128_link (IM3Module mod);
    yos_f128_link (module);

    /* Tier-2 demo binding (canary for the sidecar dispatch path). */
    m3_LinkRawFunction(module, "env", "__yos_t2_demo", "i(ii)", m3_t2_demo);

    /* clang renames user main(int, char**) to __main_argc_argv and
     * emits a wrapper main(void) that's exported. Our crt1's call to
     * main(argc, argv) therefore becomes env.__main_argc_argv. Bind
     * it to a host trampoline that calls the wasm-module's real main. */
    m3_LinkRawFunction(module, "env", "__main_argc_argv", "i(ii)",
                       m3_main_argc_argv);

    /* Variadic printf family. */
    m3_LinkRawFunction(module, "env", "printf",    "i(ii)",   m3_printf);
    m3_LinkRawFunction(module, "env", "fprintf",   "i(iii)",  m3_fprintf);
    m3_LinkRawFunction(module, "env", "sprintf",   "i(iii)",  m3_sprintf);
    m3_LinkRawFunction(module, "env", "snprintf",  "i(iiii)", m3_snprintf);
    m3_LinkRawFunction(module, "env", "vprintf",   "i(ii)",   m3_vprintf);
    m3_LinkRawFunction(module, "env", "vfprintf",  "i(iii)",  m3_vfprintf);
    m3_LinkRawFunction(module, "env", "vsprintf",  "i(iii)",  m3_vsprintf);
    m3_LinkRawFunction(module, "env", "vsnprintf", "i(iiii)", m3_vsnprintf);

    /* Auto-generated bridges for the FreeBSD-libc-name import surface.
     * For guests that import each libc fn by name (env.write, env.read,
     * env.exit, …) instead of going through __yos_syscall. Bridges
     * tolerate "function not found" so this is harmless for legacy
     * __yos_syscall-using guests too. See CLAUDE.md for the design.
     * Must run BEFORE the wildcard stub below or the stub captures
     * every name first. */
    extern int yos_brg_link_imports (IM3Module mod);
    int brg_rc = yos_brg_link_imports (module);
    if (brg_rc != 0) {
        fprintf(stderr, "yos: yos_brg_link_imports failed (rc=%d) — most "
                        "libc imports will fall through to the unresolved "
                        "stub\n", brg_rc);
    }

    /* link wildcard stub for all remaining unresolved imports */
    m3_LinkRawFunction(module, "env", "*", NULL, m3_unresolved_stub);
}

static uint8_t *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "yos: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "yos: read error: %s\n", path);
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

/* Global runtime - process table shared across all processes */
static struct yos_runtime g_runtime;

/* Forward declarations */
extern struct yos_proc *yos_proc_alloc(struct yos_runtime *rt, int32_t ppid);
extern void yos_fork_pump(struct yos_exec_ctx *ctx);
extern void yos_vfork_pump(struct yos_exec_ctx *ctx);

/* Load and prepare a wasm module. Returns 0 on success. */
static int load_wasm_module(struct yos_exec_ctx *ctx, IM3Environment env,
                            const char *path, uint8_t **out_bytes, size_t *out_size)
{
    size_t wasm_size = 0;
    uint8_t *wasm_bytes = load_file(path, &wasm_size);
    if (!wasm_bytes) return -1;

    /* Create new runtime */
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, 0);
    if (!rt) {
        fprintf(stderr, "yos: failed to create wasm3 runtime\n");
        free(wasm_bytes);
        return -1;
    }

    IM3Module module = 0;
    M3Result res = m3_ParseModule(env, &module, wasm_bytes, wasm_size);
    if (res) {
        fprintf(stderr, "yos: parse error: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }

    res = m3_LoadModule(rt, module);
    if (res) {
        fprintf(stderr, "yos: load error: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }

    /* Update context BEFORE linking imports — yos_link_imports() now
     * lazy-creates the L1 pthread host and reads ctx->runtime to get the
     * wasm3 environment. Setting it after the link left env NULL and
     * silently skipped binding the yos_pthread_* imports, which made libc
     * internals trap into the wildcard "unresolved import" stub and kill
     * the shell after a failed exec. */
    ctx->runtime = rt;
    ctx->module = module;
    rt->userdata = ctx;

    /* The pthread_host (lazy-allocated by yos_link_imports below) needs
     * the wasm bytes + size to spawn sibling runtimes for clone()ed
     * threads. Set them BEFORE the link so the host stores the right
     * pointer. */
    ctx->wasm_bytes = wasm_bytes;
    ctx->wasm_bytes_size = wasm_size;

    /* Link imports */
    yos_link_imports(module, ctx);

    /* Grow memory */
    extern M3Result ResizeMemory(IM3Runtime, uint32_t);
    /* 4096 pages * 64 KiB = 256 MiB. nvim's Lua + module dictionaries
     * easily blow past the old 16 MiB cap during startup. */
    res = ResizeMemory(rt, 4096);
    if (res) {
        fprintf(stderr, "yos: memory resize: %s\n", res);
        m3_FreeRuntime(rt);
        free(wasm_bytes);
        return -1;
    }

    /* Get memory info */
    uint32_t mem_size = 0;
    ctx->memory = m3_GetMemory(rt, &mem_size, 0);
    ctx->memory_size = mem_size;

    /* Heap starts at __heap_base — wasm-ld places it just past the data
     * segment. A hardcoded constant (e.g. 0x50000) corrupts .data for any
     * non-trivial wasm: argv strings get written into static globals, then
     * brk-driven malloc hands out chunks that overlap musl's `mal` struct
     * (oldmalloc bins/binmap), and a later trim()->bin_chunk() reads a
     * trampled bins[i].tail and traps on tail->next. nvim hits this within
     * a few thousand mallocs because its .data ends near 0x10C800.
     *
     * Tiny wasms (kernel unit tests) link without --export-all and don't
     * surface __heap_base. They fit under 0x50000, so fall back there. */
    ctx->heap_end = 0x50000;
    {
        IM3Global g = m3_FindGlobal(module, "__heap_base");
        if (g) {
            M3TaggedValue tv = { 0 };
            M3Result r = m3_GetGlobal(g, &tv);
            if (!r && tv.type == c_m3Type_i32) {
                uint32_t hb = (tv.value.i32 + 15) & ~15u;
                if (hb > ctx->heap_end) ctx->heap_end = hb;
            }
        }
    }

    /* Initialize TLS */
    uint32_t *thread_ptr = (uint32_t *)(ctx->memory + 0);
    *thread_ptr = 0x100;
    memset(ctx->memory + 0x100, 0, 256);

    extern void yos_fd_table_init(struct yos_exec_ctx *);
    yos_fd_table_init(ctx);

    *out_bytes = wasm_bytes;
    *out_size = wasm_size;
    return 0;
}

/* Free exec argv */
static void free_exec_argv(struct yos_exec_ctx *ctx)
{
    if (ctx->exec_argv) {
        for (int i = 0; i < ctx->exec_argc; i++) {
            free(ctx->exec_argv[i]);
        }
        free(ctx->exec_argv);
        ctx->exec_argv = NULL;
    }
    ctx->exec_argc = 0;
    ctx->exec_pending = 0;
}

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: yos <program.wasm>\n");
        return 1;
    }

    /* Initialize global runtime */
    memset(&g_runtime, 0, sizeof(g_runtime));
    pthread_mutex_init(&g_runtime.proc_lock, NULL);
    g_runtime.next_pid = 1;
    g_runtime.fg_pgid  = 1; /* init proc owns the tty at startup */
    g_runtime.argc = argc - 1;
    g_runtime.argv = argv + 1;
    /* Pass yos's own environment through to the wasm program. The shell
     * inside busybox / nvim plugins / etc. all read PATH, HOME, TERM,
     * etc. via getenv(); without inheriting they see an empty
     * environment and fall back to defaults that don't match what the
     * user set on the host shell. */
    {
        int ec = 0;
        if (environ) for (char **p = environ; *p; p++) ec++;
        g_runtime.envc = ec;
        g_runtime.envp = environ;
    }

    /* Initialize VFS mount table and mount /proc */
    static struct yos_mount_table mount_table;
    yos_mount_table_init(&mount_table);
    yos_mount_add(&mount_table, "/proc", &yos_procfs_ops);
    g_runtime.mount_table = &mount_table;

    /* Tier 2: load libc-pure.wasm into a sidecar wasm3 instance. The
     * exact path comes from meson via -DYOS_LIBC_PURE_PATH. If the
     * sidecar fails to load, yos still runs — Tier-2 fns will return
     * -ENOSYS at call time. */
#ifdef YOS_LIBC_PURE_PATH
    if (yos_tier2_init(YOS_LIBC_PURE_PATH) != 0) {
        fprintf(stderr,
                "yos: tier2: libc-pure.wasm unavailable, "
                "Tier-2 imports will trap\n");
    }
#endif

    IM3Environment env = m3_NewEnvironment();

    /* Allocate initial process (pid 1, ppid 0) */
    struct yos_proc *proc = yos_proc_alloc(&g_runtime, 0);
    if (!proc) {
        fprintf(stderr, "yos: failed to allocate process\n");
        return 1;
    }
    proc->state = YOS_PROC_RUNNING;
    /* Record the main thread so kill(pid=1, sig) / tkill / tgkill from
     * inside the wasm can resolve the guest pid back to a real host
     * thread via pthread_kill(). Forked procs get this filled in by
     * the fork-thread spawn path. */
    proc->thread = pthread_self();

    /* Initialize process info */
    if (syscall(__NR_getcwd, proc->cwd, sizeof(proc->cwd)) < 0)
        strcpy(proc->cwd, "/");
    strncpy(proc->exe, argv[1], PATH_MAX - 1);
    const char *slash = strrchr(argv[1], '/');
    strncpy(proc->comm, slash ? slash + 1 : argv[1], sizeof(proc->comm) - 1);
    proc->cmdline = argv + 1;
    proc->cmdline_argc = argc - 1;

    /* Set up exec context */
    struct yos_exec_ctx ctx = {0};
    ctx.rt = &g_runtime;
    ctx.proc = proc;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;
    ctx.envc = g_runtime.envc;
    ctx.envp = g_runtime.envp;
    pthread_mutex_init(&ctx.mem_lock, NULL);
    strcpy(ctx.cwd, proc->cwd);

    /* Load initial module */
    size_t wasm_size = 0;
    uint8_t *wasm_bytes = NULL;
    if (load_wasm_module(&ctx, env, argv[1], &wasm_bytes, &wasm_size) != 0) {
        return 1;
    }
    ctx.wasm_bytes = wasm_bytes;
    ctx.wasm_bytes_size = wasm_size;

    ydebug("main: ctx=%p, rt=%p\n", (void*)&ctx, ctx.runtime);

    /* Main exec loop - run _start, reload module if exec happens */
    for (;;) {
        IM3Function start_fn;
        M3Result res = m3_FindFunction(&start_fn, ctx.runtime, "_start");
        if (res) {
            fprintf(stderr, "yos: _start not found: %s\n", res);
            return 1;
        }

        res = m3_CallV(start_fn);

        /* Drain pumps until quiescent. Each pump's internal m3_CallV(_start)
         * resumes wasm, which may trigger another pump (e.g. an ash command
         * loop does setjmp → rewind → fork → setjmp ...). Loop until no
         * fork/setjmp/longjmp event is pending. */
        for (;;) {
            int progress = 0;
            if (ctx.fork_pending) { yos_fork_pump(&ctx);  progress = 1; }
            if (ctx.setjmp_pending || ctx.longjmp_pending) {
                yos_setjmp_pump(&ctx);
                progress = 1;
            }
            if (!progress) break;
        }
        /* Surface a trap from inside the pump as the outer res so the
         * "main returned cleanly" path doesn't swallow it. */
        if (ctx.pump_trap) { res = ctx.pump_trap; ctx.pump_trap = NULL; }

        /* Check if exec happened */
        if (!ctx.exec_pending) {
            if (res) {
                ydebug("trap: %s\n", res);
                IM3BacktraceInfo bt = m3_GetBacktrace(ctx.runtime);
                if (bt) {
                    IM3BacktraceFrame f = bt->frames;
                    while (f) {
                        ydebug("  at %s\n", f->function ? m3_GetFunctionName(f->function) : "?");
                        f = f->next;
                    }
                }
            }
            break;
        }

        /* Handle exec - load new module */
        ydebug("exec: loading %s\n", ctx.exec_path);

        m3_FreeRuntime(ctx.runtime);
        free(wasm_bytes);

        ctx.argc = ctx.exec_argc;
        ctx.argv = ctx.exec_argv;
        /* If the wasm passed an envp, install it as the new env. NULL
         * means "no env replacement" — POSIX exec says the new image
         * gets the caller's envp, but inheriting the existing ctx
         * matches what musl does when given environ as envp. */
        if (ctx.exec_envp) {
            ctx.envc = ctx.exec_envc;
            ctx.envp = ctx.exec_envp;
        }

        if (load_wasm_module(&ctx, env, ctx.exec_path, &wasm_bytes, &wasm_size) != 0) {
            fprintf(stderr, "yos: exec failed to load %s\n", ctx.exec_path);
            free_exec_argv(&ctx);
            return 127;
        }
        ctx.wasm_bytes = wasm_bytes;
        ctx.wasm_bytes_size = wasm_size;
        ctx.free_count = 0;

        ctx.exec_pending = 0;
        ctx.exec_argv = NULL;
        ctx.exec_argc = 0;
        ctx.exec_envp = NULL;
        ctx.exec_envc = 0;

        ydebug("exec: loaded %s, argc=%d\n", ctx.exec_path, ctx.argc);
    }

    m3_FreeRuntime(ctx.runtime);
    m3_FreeEnvironment(env);
    free(wasm_bytes);
    return 0;
}
