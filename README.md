# yos

**A host-side FreeBSD-libc emulator for wasm32 programs.**

yos lets you run unmodified wasm32 binaries — built against FreeBSD
libc headers — on Linux, macOS / iOS / tvOS, or FreeBSD. The wasm
guest only ever sees a FreeBSD-shaped userspace; yos translates each
libc call into the host's native libc on the way through.

## Why

We want one wasm binary that runs across desktop, mobile, and embedded
without recompilation, and without dragging the Linux kernel ABI along
for the ride. Apple's app-store policies forbid direct kernel calls.
FreeBSD's userspace ABI is BSD-derived, stable, and small enough to
emulate end-to-end. Targeting *FreeBSD libc* (not WASI, not musl, not
Linux syscalls) gives us:

- One ABI to compile against. The wasm binary embeds no host
  assumptions — every libc call is a wasm import on `env.<name>`.
- A natural fit for iOS/tvOS via libSystem, which shares its BSD
  heritage with FreeBSD's libc.
- Clean licensing. The host stack is wasm3 (MIT) + mimalloc (MIT) +
  FreeBSD libc *headers* (BSD-2-Clause) + the host's own libc. yos
  itself is BSL.

## What runs today

The umbrella package (`nix build .#all`) ships a host yos runtime and
a working wasm userspace built against FreeBSD libc:

- **zsh 5.9** — interactive shell, scripting, pipes, command
  substitution, env passthrough, fork+exec.
- **neovim 0.10.4** — full TUI, runtime tree, plugin loader.
- **FreeBSD coreutils — first batch (run cleanly today):** cat, echo,
  pwd, mkdir, ln, head, hostname, id, kill, mktemp, realpath, sleep,
  sync, test, touch, true, false, uniq, yes, basename, dirname, rmdir.

- **FreeBSD coreutils — second batch (build cleanly; runtime
  partial):** awk, cut, df, du, find, grep, sed, sort, tr, wc, xargs.

  These compile and link, but most trap or misbehave at runtime
  against the current yos host bridge. Status pinned by
  `tests/integration/freebsd-tools/`:

  | Tool   | Runtime | Failure mode (xfail in test suite)                 |
  |--------|---------|----------------------------------------------------|
  | wc     | partial | line + byte counts correct; words always 1         |
  | df     | partial | runs clean but prints nothing (0 mounts in stub)   |
  | awk    | broken  | lexer reads only first byte (rune-locale gap)      |
  | cut    | broken  | empty / `\n\n`-only output (rune-locale gap)       |
  | tr     | broken  | unresolved import `env.mergesort`                  |
  | grep   | broken  | no matches (regex char-class lookup hits same gap) |
  | sed    | broken  | mimalloc free-list corruption                      |
  | sort   | broken  | exits ENOENT in setlocale path                     |
  | xargs  | broken  | "no free pid slot" — proc-table slot not recycled  |
  | find   | broken  | unresolved import `env.fts_open`                   |
  | du     | broken  | same `env.fts_open` gap as find                    |

  The shared root cause for many of these is the wasm guest's
  `<_ctype.h>` macros deref'ing `_CurrentRuneLocale->__runetype[c]` —
  yos doesn't initialise the FreeBSD rune-locale at startup, so every
  `isspace`/`isalpha`/`isdigit` returns 0 and the tools' lexers /
  parsers / delimiter walkers all silently mis-classify every byte.
  Test suite tracks each gap with a docstring + xfail entry; flipping
  any test to UNEXPECTEDPASS marks a real regression-net win.

  Build-time machinery the second batch added that's unconditionally
  green: bison generates `getdate.c` for find and `awkgram.c` for
  awk; awk's `proctab.c` is emitted by a host-side `maketab` build;
  sort vendors `md5c.c` from `lib/libmd`; df + wc link a 200-line
  text-only libxo shim. Plus `src/yos/impl/freebsd_userland.c` adds
  host-side bridges for `strdup`/`strerror`/`asprintf`/`getmntinfo`/
  `getbsize`/`strtonum`/`fgetln`/`getprogname` etc., which the
  bridge.py auto-stubs would otherwise return NULL for.

- **Not yet ported (dependency cost too high for one tool change):**
  - **ps, top** — need `libkvm` + a kernel-style proc table; yos's
    proc table is host-side, would need a custom `machine.c` reading
    yos's procfs.
  - **tar** — needs libarchive (~500+ files) — own derivation.
  - **gzip** — needs zlib + lzma + zstd, three separate ports.

Each lives in `result/libexec/<name>` as a bare wasm module that
yos's exec bridge can load directly. Shell-script wrappers in
`result/bin/<name>` make them runnable from a host shell. A sandbox
wrapper at `result/bin/yos-shell` (and `nix run .#`) drops you into an
interactive zsh under yos with `PATH` scoped to `libexec/` only.

## Quick start

```sh
# Build the umbrella package (yos + zsh + nvim + tools).
nix build .#all

# Sandboxed wasm-zsh under yos, host PATH stripped.
nix run .#

# Or use the convenience launcher:
./tools/yos.sh                 # interactive zsh
./tools/yos.sh nvim file.txt   # any wasm tool, passthrough args
./tools/yos.sh -c 'echo hi'    # one-shot zsh
```

## How yos resolves a libc call

Every libc function the wasm guest imports goes through a 3-tier
ladder. yos picks the lowest tier that gives FreeBSD-correct
behaviour:

1. **Host-libc passthrough.** A bridge function translates wasm
   pointers to host pointers, calls the host's libc with the same
   name, returns. This covers most pure / stateless functions —
   `strlen`, `memcpy`, `read`, `sin`, `qsort`, …
2. **FreeBSD source compiled to wasm32** (Tier 2). For functions
   whose host equivalent has the wrong shape (variadic printf,
   FreeBSD-specific locale stuff), yos compiles the FreeBSD libc
   source files to wasm and dispatches to a sidecar wasm3 instance.
3. **Hand-written.** State-bound operations — `fork`, `execve`,
   `waitpid`, `pthread_create`, `mmap`/`brk`, fd table, `mkdtemp`,
   `realpath` — are implemented in C under `src/yos/impl/`.

Bridges for tier 1 are **autogenerated** from the FreeBSD libc
headers. libclang walks both the guest's FreeBSD-i386 headers and the
host's libc headers, emits structured YAML deltas (widen / narrow /
struct-convert / pointer-descent / kind-mismatch), and the generator
emits one bridge per function. Adding a new libc fn is usually a
*header-only* change.

## Architecture, in one diagram

```
+--------------------+   wasm imports on env.<freebsd-libc-name>
|  wasm guest        |   (e.g. env.open, env.write, env.fork,
|  built against     |    env.pthread_create)
|  FreeBSD libc      |
|  headers, wasm32   |
+---------+----------+
          |
          v
+--------------------+   per-import m3ApiRawFunction wrapper:
|  yos_brg_<name>    |   - pop wasm-ABI args
|  (auto-generated   |   - translate wasm offsets -> host pointers
|  from extracted    |   - convert FreeBSD struct layout -> host
|  type yaml)        |   - errno remap
+---------+----------+
          |
   +------+------+
   |             |
   v             v
+----------+  +-------------+
| yos      |  | host libc   |
| subsystem|  | (glibc /    |
| (mem/    |  |  libSystem /|
|  proc/   |  |  FreeBSD    |
|  pthread |  |  libc)      |
|  /vfs)   |  +-------------+
+----------+
```

For the deeper "what's autogenerated, what's hand-written, why, and
how the Nix packaging fits" picture, see [docs/design.md](docs/design.md).

## Repo layout

```
src/yos/                yos host runtime
  main.c                  binary entry, links wasm3 imports
  impl/                   hand-written subsystems (proc, sig, vfs,
                          file, env, mem, pthread, …)
  codegen/                bridge.py + the YAML it emits from
build-tools/
  freebsd/                fetched FreeBSD source, curated header tree
  host-libc/              snapshot of the host's libc include path
  wasm-pkg/configs/<n>/   per-package recipes (build.sh)
  sysroot/                wasm32 sysroot skeleton (crt1, libyos_stubs)
  toolchain/              wasm-clang shim
  wasm3/vendored/         vendored wasm3 fork (sibling runtime, atomics)
  mimalloc/               vendored mimalloc (host-side allocator)
nixpkgs/                  Nix package tree
  default.nix             top-level — wires yos/sysroot/toolchain
                          + `all` umbrella
  yos/                    host runtime derivation
  sysroot/                wasm32 sysroot derivation
  toolchain/              clang/wasm-ld/wasm-opt + wasm-clang shim
  pkgs/<name>/            per-package nix expressions
  lib/                    shared builders (build-recipe, build-freebsd-tool)
tests/integration/        end-to-end pty + scripted tests for yos's
                          host runtime via real wasm guests
tools/                    convenience scripts (yos.sh, wasm-pkg.sh)
flake.nix                 single source of truth for nix outputs
```

## Building things yourself

The Nix flake is the canonical build interface:

```sh
nix build .#yos              # host runtime
nix build .#sysroot          # wasm32 sysroot
nix build .#toolchain        # wasm-clang + wasm-ld + wasm-opt
nix build .#zsh              # zsh wasm
nix build .#nvim             # neovim wasm + runtime tree
nix build .#freebsd-tools    # bundled coreutils
nix build .#all              # umbrella: yos + every wasm port

nix shell .#all              # PATH includes the umbrella's bin/
nix run   .#                 # apps.default = the wasm-zsh sandbox
```

For development without Nix:

```sh
meson setup build-linux
ninja -C build-linux
meson test -C build-linux           # runs the integration suite
```

## Status

- yos host runtime: building cleanly on Linux, macOS, FreeBSD;
  most-common bridges either tier-1-passthrough or hand-written.
- wasm packages: zsh, nvim, freebsd-tools all in the umbrella.
- Known gaps tracked under `tests/integration/zsh/meson.build` and
  the per-package recipes — see the comments next to `xfail : true`.
- Future ports: bash 5, FreeBSD `bin/sh`, harder coreutils
  (`ls`, `mv`, `cp`).

## Licence

BSL. See `LICENSE`.
