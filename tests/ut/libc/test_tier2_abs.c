/*
 * test_tier2_abs.c — first generator-driven Tier-2 function.
 *
 * WHAT this verifies:
 *   `int abs(int)` is listed under `from_freebsd_src:` in
 *   src/yos/codegen/hooks.yaml. bridge.py emits an m3w_abs
 *   wrapper whose body calls FreeBSD's lib/libc/stdlib/abs.c
 *   (compiled into libc-pure.wasm) via the sidecar wasm3
 *   instance. The guest imports env.abs from yos_imports.h and
 *   gets that path automatically — no hand-written attribute.
 *
 * WHY this matters:
 *   This is the first END-TO-END generator-driven Tier-2 path:
 *   FreeBSD source (verbatim) → libc-pure.wasm (built by
 *   build-tools/freebsd-libc/) → sidecar wasm3 instance → guest
 *   import resolution → result back to guest. If this works,
 *   adding any other scalar-only FreeBSD libc function is one
 *   yaml line + one source-file entry.
 *
 *   Once we add cross-runtime memory marshalling, pointer-arg
 *   functions (strlcpy, ...) follow the same path.
 *
 * Expected: exit 0, stdout contains "abs ok".
 */

#include "yos_imports.h"

void _start(void) {
    /* abs(-7) must be 7. yos_imports.h declares it as
     *   int abs(int);
     * with import attributes; clang emits env.abs. */
    if (abs(-7) != 7) {
        static const char fail[] = "abs wrong\n";
        write(1, fail, sizeof(fail) - 1);
        _exit(1);
    }
    if (abs(42) != 42) {
        static const char fail[] = "abs(positive) wrong\n";
        write(1, fail, sizeof(fail) - 1);
        _exit(2);
    }
    static const char ok[] = "abs ok\n";
    write(1, ok, sizeof(ok) - 1);
    _exit(0);
}
