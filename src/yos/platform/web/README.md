# yos in the browser — architecture proof

This slice proves the disputed point concretely: **no interpreter in the
browser, and the bridge is wasm, not JS.**

The browser is itself a wasm engine, so the guest `.wasm` runs on it
natively. yos's value — the `env.<freebsd-libc-name>` bridge layer plus
the stateful subsystems — also compiles to wasm and **shares the guest's
linear memory**, so a guest pointer and a bridge pointer are the same
address. Calls from guest to bridge are wasm→wasm at engine speed. The
only thing that crosses into JS is the irreducible sandbox edge: effects
wasm physically cannot perform alone (push bytes at a terminal, spawn a
Worker, touch storage, read the clock).

```
guest.wasm  ──env.write / env.strlen──▶  bridge.wasm        (wasm → wasm, native)
   │                                          │
   └──────────── one shared WebAssembly.Memory ──────────────┘
                                              │
                                  host.write_bytes            (the ONLY JS)
```

## What runs

- `guest.c` → `guest.wasm` — the app. Imports `env.write`, `env.strlen`,
  `env.memory`. Knows nothing about the bridge.
- `bridge.c` → `bridge.wasm` — the yos-host bridge. Exports `write` /
  `strlen`, imports `host.write_bytes` (the single host effect) and the
  shared `env.memory`. `strlen` is a **pure wasm bridge**: it reads the
  guest string out of shared memory and computes with zero JS. `write`
  is the Tier-1 shape: translate (no-op, shared memory) → dispatch to the
  one host effect → return.
- `run.mjs` — headless harness (`node run.mjs`). Owns the one Memory,
  wires guest↔bridge, asserts the output.
- `index.html` + `serve.sh` — the same wiring in a tab.
- `shell_demo.c` → `shell_demo.wasm` — an **interactive** guest: a
  minimal line shell you type into. Push-driven (`feed_byte` per
  keystroke), so no blocking `read()` yet. NOT zsh — it is the skeleton
  the real guest plugs into.
- `terminal.html` — a real **xterm.js** terminal wired to that guest
  through the wasm bridge. `shell_test.mjs` checks the same guest+bridge
  headlessly.

## Run it

```sh
nix develop ../../../../#default --command ./build.sh         # → *.wasm
nix develop ../../../../#default --command node run.mjs        # headless proof: PASS
nix develop ../../../../#default --command node shell_test.mjs # headless shell: all ok
nix develop ../../../../#default --command ./serve.sh          # browser
#   then open http://127.0.0.1:8099/terminal.html and type:
#   `help`, `echo hi`, `clear`

# Drive the real page in headless Chrome (no npm deps; speaks the
# DevTools Protocol over node's built-in WebSocket). Types into the
# guest, asserts the rendered output. YOS_SHOT=path captures a PNG.
nix develop ../../../../#default --command \
  bash -c 'YOS_CHROME=google-chrome-stable node browser_test.mjs'
```

## The gap to real zsh (measured, not guessed)

`nix build .#zsh` produces a 2 MB wasm32 zsh that **imports 169
`env.*` functions** and exports its own memory + the asyncify entry
points + `_start`. Those 169 are the work to boot it in a tab:

| group | count | examples | browser need |
|---|---|---|---|
| pure / stateless | ~50 | `memcpy` `strlen` `qsort` `snprintf` `strtoul` | trivial wasm bridges |
| FILE\* stdio | ~20 | `fopen` `fread` `fwrite` `fflush` `open_memstream` | FILE\* table |
| filesystem | ~25 | `open` `read` `write` `stat` `opendir` `readlink` | virtual FS (no host FS in a tab) |
| process + fork | ~15 | `fork` `execve` `wait3` `kill` `setpgid` | asyncify-fork without wasm3 |
| tty + fd | ~12 | `tcgetattr` `tcsetattr` `ioctl` `dup2` `poll` `select` | a JS PTY |
| signals / creds / time / env | rest | `sigaction` `getpwnam` `clock_gettime` `getenv` | per-area shims |

zsh also exports `memory` (it owns it), so to share memory the
production path either rebuilds zsh with `--import-memory` (embedder owns
the Memory, as this proof does) or has the bridge import zsh's exported
memory via deferred binding. Settle that with the layout decision below.

## How memory is shared (the load-bearing decision)

Both modules are linked `-Wl,--import-memory`, so neither owns the
memory — the embedder creates ONE `WebAssembly.Memory` and hands it to
both. That breaks the instantiation cycle (guest imports the bridge's
exports; the bridge imports the guest's memory would be circular) and is
exactly how Emscripten's dynamic linking shares memory.

The one unsolved-at-scale question this slice **stands in for, does not
solve**: the bridge's own static data + stack must not collide with the
guest's. Here the bridge is parked at a high `--global-base` (8 MiB) and
the guest's footprint is tiny, so they never overlap. The real yos-host
has real state (proc table, fd table, mmap free-list) and the guest
manages its full linear-memory layout (brk heap low, mmap arena high), so
the production answer is one of:

1. **Dynamic-linking memory base** — assign the yos-host module a runtime
   `__memory_base` in a region the guest's allocator is told to avoid
   (Emscripten MAIN/SIDE module model).
2. **Multi-memory** — yos-host keeps memory 0 for its private state and
   addresses the guest's memory as memory 1. Cleaner (no carve-out), and
   now shipping in V8/Firefox, but C/lld support is still young.

Settle that before porting the subsystems. Everything above this line is
proven to work.

## What this slice deliberately omits (the rest of the port, in order)

1. **Real `env.*` surface** — retarget `build-tools/api-generate/bridge.py`
   to emit these wasm bridges instead of native C ones; the
   extract→compare→analyse pipeline is unchanged.
2. **Virtual backends for stateful libc** — a JS/in-wasm filesystem behind
   the existing `src/yos/vfs/` seam; sockets over WebSocket or `ENOSYS`.
3. **Guest threads** — guest compiled with wasm threads/atomics + a
   `shared` Memory; `pthread_create` spawns a Web Worker. Needs the
   COOP/COEP headers `serve.sh` already sends.
4. **fork** — asyncify (already a guest-side transform, runs native) +
   a new Worker with a fresh, non-shared Memory + snapshot copy. Mirrors
   the native split: threads share memory, fork gets its own.
