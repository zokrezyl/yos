/*
 * tier2_demo.c — proof-of-concept Tier-2 function.
 *
 * A trivial pure function compiled to wasm32, embedded in
 * libc-pure.wasm, loaded by yos as a sidecar. Exists ONLY to pin
 * the plumbing: the yos host binary loads libc-pure.wasm into a
 * secondary wasm3 instance; a bridge body for an env import on
 * the GUEST side dispatches into this secondary instance and
 * forwards the call. Once we have a real FreeBSD source piece in
 * libc-pure.wasm (e.g. lib/libc/string/strlcpy.c) we can drop
 * this file.
 */

__attribute__((visibility("default")))
int __yos_t2_demo(int a, int b) { return a + b; }
