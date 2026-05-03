/*
 * test_tier2_sidecar.c — Tier-2 sidecar dispatch canary.
 *
 * WHAT this verifies:
 *   The wasm guest imports env.__yos_t2_demo(int, int) -> int. yos's
 *   bridge for that import dispatches into the sidecar wasm3 runtime
 *   that hosts libc-pure.wasm (built by build-tools/freebsd-libc/),
 *   where the actual implementation `int __yos_t2_demo(int, int)`
 *   lives. The sidecar runs the function and returns the result; the
 *   bridge propagates it back to the guest.
 *
 * WHY this matters:
 *   This is the canary for the entire Tier-2 plumbing — sidecar
 *   build, sidecar load at yos startup, function lookup, cross-
 *   runtime call, result marshalling. Once it's green, swapping
 *   __yos_t2_demo for a real FreeBSD libc piece (strlcpy, qsort,
 *   printf, …) is a matter of adding the .c file to libc-pure.wasm
 *   and registering a `from_freebsd_src` entry in hooks.yaml. The
 *   plumbing under it is the same.
 *
 * Expected: exit 7, stdout contains "tier2 ok".
 */

__attribute__((import_module("env"), import_name("__yos_t2_demo")))
int __yos_t2_demo(int a, int b);

__attribute__((import_module("env"), import_name("write")))
int write(int, const void *, unsigned int);

__attribute__((import_module("env"), import_name("exit")))
__attribute__((noreturn))
void _exit(int);

void _start(void) {
    int r = __yos_t2_demo(3, 4);
    /* 3+4 == 7 — also conveniently the exit code we propagate so the
     * test runner sees both signals (stdout substring + exit). */
    if (r != 7) {
        static const char fail[] = "tier2 wrong result\n";
        write(1, fail, sizeof(fail) - 1);
        _exit(1);
    }
    static const char ok[] = "tier2 ok\n";
    write(1, ok, sizeof(ok) - 1);
    _exit(r);
}
