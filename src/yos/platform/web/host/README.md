# Browser convergence host (`yos-host.wasm`) — Phase 0

This directory holds the **converged browser host**: the yos C runtime compiled
to a browser-targeted wasm module with Emscripten, so the browser runs the same
FreeBSD-shaped C code as desktop instead of the hand-written JavaScript libc in
[`../yos_proc.mjs`](../yos_proc.mjs). It implements epic
[#33](https://github.com/zokrezyl/yos/issues/33), following `docs/browser.md`
(Architecture A — wasm3-in-wasm).

It is kept **separate** from the legacy `../*.mjs` prototypes on purpose: those
are frozen; this is the path that replaces them once parity gates pass.

## Phase 0 (issue [#34](https://github.com/zokrezyl/yos/issues/34)) — done

Phase 0 proves the single enabling fact: **the wasm3 interpreter, itself
compiled to a browser wasm module, can execute a guest wasm** — no JavaScript
reimplementation of any guest libc.

- `smoke_guest.c` — a trivial wasm32 guest with **zero imports** that exports
  `int compute(void)` returning 42. Zero imports is the point: there is nothing
  for JavaScript to satisfy, so the guest cannot be secretly served by JS.
- `phase0_host.c` — the Emscripten host. `main()` drives wasm3 (parse →
  load → find `compute` → call → read result) over the guest embedded as a C
  byte array, and prints the result. Every wasm3 `M3Result` is checked, so a
  failure to execute returns non-zero.
- `phase0_yperf_stub.c` — no-op definitions of the three `yperf_*` profiling
  symbols the vendored wasm3 fork references unconditionally (disabled by
  default; a later phase can swap in the real `src/yos/yperf/yperf.c`).
- `build-phase0.sh` — builds the guest, embeds it (`smoke_guest_wasm.h`), and
  emcc-compiles `phase0_host.c` + the vendored wasm3 sources into
  `yos-host-phase0.mjs` (+ `.wasm`).
- `phase0_smoke.mjs` — the gate: loads the host in Node, captures its output
  through a minimal `print` hook, and fails the build unless wasm3 actually ran
  the guest and returned 42.

### Build & test

```sh
make browser-host-phase0        # build the Phase 0 host artifact
make test-browser-host-phase0   # build + run the Node smoke test
```

Expected smoke output:

```
phase0: wasm3 executed guest, compute()=42
phase0 smoke: PASS — wasm3-in-wasm executed the embedded guest
```

### Toolchain

The host is compiled with **nix-provided Emscripten** (nixpkgs 24.11 →
emscripten 3.1.64), obtained through the flake dev shell — the Makefile targets
run the build under `nix develop`, so no separate emsdk install is required. The
guest is compiled with the system `clang` (any clang with the wasm32 backend).

Two notes for anyone running `build-phase0.sh` by hand:

- Do it inside `nix develop` (or `nix shell github:NixOS/nixpkgs/nixos-24.11#emscripten`)
  so a working `emcc` is on PATH.
- The build links at `-O2`, not `-O3`: the packaged emscripten frontend emits an
  `-O3`-only binaryen flag (`--no-stack-ir`) its pinned `wasm-opt` rejects. `-O2`
  produces the same sibling-call dispatch wasm3 needs and links cleanly.

Overrides honored by the build script: `YOS_WASM_CLANG` (guest compiler,
default `clang`), `EMCC` (default `emcc`), `PYTHON` (default `python3`, used to
generate the embed header).

## What comes next

Phase 0 only runs a computation. The FreeBSD libc surface is untouched until:

- **#35 (Phase 1a)** — wire a few guest imports (`write`/`getpid`/`exit`)
  through the yos C bridge shape, not JS.
- **#36 (Phase 1b)** — compile the generated bridge + `impl/*.c` + `vfs/*.c`
  into the host wasm.
- **#37 (Phase 2)** — point the parity harness at this host, same tool artifacts
  as desktop.
- **#38 (Phase 3)** — cooperative fork/process model.
- **#39 (Phase 4)** — freeze the JS host, switch the browser default.
