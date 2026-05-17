/* impl/libpython.c — host-side bridges that expose libpython 3.12 to
 * the wasm guest as env.Py_* imports.
 *
 * Architecture: the yos host binary links against libpython3.12.so
 * (or .a) at compile/link time. The wasm guest carries no Python
 * code — just a small driver that calls Py_Initialize(),
 * PyRun_SimpleString("..."), Py_Finalize() via env imports. yos
 * resolves those imports to the host wrappers below.
 *
 * Why this layout vs cross-compiling CPython to wasm32:
 *   - speed: native interpreter, no double interpretation through wasm3
 *   - size: tens-of-MB host libpython is shared with any other process
 *           on the host, vs each .wasm guest carrying its own copy
 *   - cross-compile pain: zero — we use the host's already-built libpython
 *   - cost: yos binary needs libpython3.12 at link time (per-platform),
 *           and the security boundary is now "whatever Python can do"
 *           (same as libc — see CLAUDE.md security note).
 *
 * Bridge convention here: opaque host pointers passed as i32 handles.
 * We don't (yet) implement a handle table — these three functions take
 * no PyObject* args. Once we bridge PyObject_*, we'll add one.
 */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"
#include "yos/ydebug.h"

/* libpython 3.12 forward decls. We deliberately don't #include
 * <Python.h> here — it pulls in the whole Python C API plus a pile of
 * macros (Py_INCREF, ...) that pollute every translation unit that
 * also includes this header. Forward-declare the precise subset we
 * use; host linker resolves them against libpython3.12.so.
 *
 * Per-guest isolation: every wasm guest gets its OWN Python
 * subinterpreter (Py_NewInterpreter), giving it independent
 * sys.modules, its own builtins, and (3.12+, PEP 684) its own GIL.
 * Without subinterpreters two guests sharing one libpython would see
 * each other's global state — `import os; os.path = trap` in guest A
 * would poison guest B's next `os.path.join(...)`. */
typedef struct _ts PyThreadState;
extern void Py_Initialize(void);
extern void Py_Finalize(void);
extern int  PyRun_SimpleString(const char *command);
extern PyThreadState *Py_NewInterpreter(void);
extern void Py_EndInterpreter(PyThreadState *);
extern PyThreadState *PyThreadState_Swap(PyThreadState *);
extern int  Py_IsInitialized(void);

/* One-shot main-interpreter init. Py_Initialize is documented as
 * idempotent (no-op on second call), but we still want exactly one
 * call ever — guards the host-global runtime. */
static void libpython_ensure_initialized(void)
{
    static int inited = 0;
    if (inited) return;
    if (!Py_IsInitialized()) Py_Initialize();
    inited = 1;
}

/* Pull out the per-guest subinterpreter tstate held in ctx. Stored
 * as void* in struct yos_exec_ctx (avoids leaking PyThreadState into
 * the ctx struct definition). NULL means "this guest hasn't called
 * Py_Initialize yet". */
static PyThreadState *libpython_ctx_tstate(struct yos_exec_ctx *ctx)
{
    return ctx ? (PyThreadState *)ctx->py_tstate : NULL;
}

static void libpython_ctx_set_tstate(struct yos_exec_ctx *ctx, PyThreadState *ts)
{
    if (ctx) ctx->py_tstate = (void *)ts;
}

/* env.Py_Initialize — () → void.
 *
 * Two-stage per-guest setup:
 *   1. Ensure the host-process-wide CPython runtime exists
 *      (Py_Initialize, idempotent — runs the main interpreter).
 *   2. Create a SUBINTERPRETER for this wasm guest, store its
 *      PyThreadState* in the ctx. Subsequent Py_RunString from
 *      this guest swaps to that tstate first, so two guests
 *      see independent sys.modules, builtins, etc.
 *
 * Without (2): a malicious guest could `import os; os.path = X`
 * and poison the next guest's stdlib. With (2): each guest gets
 * its own modules dict; cross-guest mutation impossible. */
static const void *m3_yos_Py_Initialize(IM3Runtime runtime, IM3ImportContext _ctx,
                                        uint64_t *_sp, void *_mem)
{
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    (void)_ctx; (void)_sp; (void)_mem;
    ydebug("Py_Initialize (per-guest subinterpreter)\n");
    libpython_ensure_initialized();
    if (libpython_ctx_tstate(ctx) == NULL) {
        PyThreadState *prev = PyThreadState_Swap(NULL);
        PyThreadState *ts = Py_NewInterpreter();
        if (ts) {
            libpython_ctx_set_tstate(ctx, ts);
            /* Subinterpreter create leaves it as the current tstate;
             * swap back so the next call site does an explicit swap. */
            PyThreadState_Swap(prev);
        } else {
            PyThreadState_Swap(prev);
        }
    }
    return NULL;
}

/* env.Py_Finalize — () → void.
 *
 * Tear down THIS guest's subinterpreter only. The main interpreter
 * and the host-process runtime stay alive for other guests; we never
 * call Py_Finalize at the host level (would kill libpython for
 * every guest at once). */
static const void *m3_yos_Py_Finalize(IM3Runtime runtime, IM3ImportContext _ctx,
                                      uint64_t *_sp, void *_mem)
{
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    (void)_ctx; (void)_sp; (void)_mem;
    PyThreadState *ts = libpython_ctx_tstate(ctx);
    ydebug("Py_Finalize (subinterpreter %p)\n", (void *)ts);
    if (ts) {
        PyThreadState *prev = PyThreadState_Swap(ts);
        Py_EndInterpreter(ts);   /* consumes ts */
        libpython_ctx_set_tstate(ctx, NULL);
        /* Py_EndInterpreter cleared the slot; prev (if non-NULL) is
         * stale only if it was for the same ts we just killed.
         * Reattach the main tstate by swapping NULL → NULL is fine. */
        if (prev && prev != ts) PyThreadState_Swap(prev);
    }
    return NULL;
}

/* env.PyRun_SimpleString — (const char *) → int.
 * Wasm signature: i(i) — takes one i32 (wasm memory offset for the
 * command string), returns one i32 (libpython's return code).
 *
 * Swaps to THIS guest's subinterpreter tstate first so the running
 * code sees its own sys.modules / builtins. */
static const void *m3_yos_PyRun_SimpleString(IM3Runtime runtime,
                                             IM3ImportContext _ctx,
                                             uint64_t *_sp, void *_mem)
{
    struct yos_exec_ctx *ctx = (struct yos_exec_ctx *)m3_GetUserData(runtime);
    (void)_ctx;
    uint32_t cmd_off = (uint32_t)_sp[1];
    const char *cmd = "";
    if (ctx && ctx->memory && cmd_off < ctx->memory_size) {
        cmd = (const char *)(ctx->memory + cmd_off);
    }
    ydebug("PyRun_SimpleString(\"%.80s\")\n", cmd);

    PyThreadState *guest = libpython_ctx_tstate(ctx);
    PyThreadState *prev = guest ? PyThreadState_Swap(guest) : NULL;
    int rc = PyRun_SimpleString(cmd);
    if (guest) PyThreadState_Swap(prev);
    _sp[0] = (uint64_t)(uint32_t)rc;
    return NULL;
}

/* Public entry point — called from main.c during import linkage. */
void yos_libpython_link(IM3Module mod)
{
    if (!mod) return;
    /* m3 link signatures: 'v'=void, 'i'=i32, 'I'=i64, 'f'=f32, 'F'=f64.
     * Format is "<ret>(<args>)" — i(i) = i32 fn(i32). */
    m3_LinkRawFunction(mod, "env", "Py_Initialize",    "v()",  m3_yos_Py_Initialize);
    m3_LinkRawFunction(mod, "env", "Py_Finalize",      "v()",  m3_yos_Py_Finalize);
    m3_LinkRawFunction(mod, "env", "PyRun_SimpleString","i(i)", m3_yos_PyRun_SimpleString);
}
