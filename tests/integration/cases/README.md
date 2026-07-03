# Shared command case table

One declarative case table, two backends, libvterm as the shared oracle.

Each `*.json` file here holds either a single case object or an array of them.
A **case** describes one command invocation and what a human should see on the
screen after it runs. The same data drives both runners so a case authored once
is asserted on **both**:

- the **native** `yos` binary (the source of truth), and
- the **browser process engine** (`yos_proc.mjs`, cooperative;
  `mt_engine.mjs` for threads).

Both backends run the **same** tool wasm (`<libexec>/<argv[0]>`), capture the
raw terminal stream, and render it through `vterm_grid.c` (libvterm) into a
faithful rows×cols grid — so any grid difference is purely the engine, not a
different build of the tool.

The runner is `src/yos/platform/web/browser-parity-suite.mjs`; run it with
`make test-browser-parity`.

## Case schema

```jsonc
{
  "id": "grep-fixed-match",      // unique, human-readable
  "argv": ["grep", "apple"],     // argv[0] resolves to <libexec>/grep.wasm
  "stdin": "apple\nbanana\napple\n",   // optional; fed to both backends as a pipe
  "cols": 80,                    // optional (default 80) — grid width
  "rows": 24,                    // optional (default 24) — grid height
  "interactive": false,          // optional (default false) — reserved for TUI cases
  "expect": {                    // any subset; ALL present matchers must hold
    "exitCode": 0,               // guest exit code
    "rawStdout": "apple\napple\n",       // exact stdout bytes (CR-normalised)
    "stdoutContains": "apple",           // stdout substring
    "gridContains": ["apple"],           // string | array — rendered grid contains each
    "gridRow": { "row": 0, "equals": "apple" },   // a specific rendered row (equals | contains)
    "goldenGrid": "golden/grep.grid"     // rendered grid equals a committed golden file
  }
}
```

## How a case is classified

The `expect` block is the correctness criterion (the oracle). For each case:

1. Run it on the **browser engine**, render its stream to a grid, evaluate
   `expect`. If it holds → **PASS**.
2. Otherwise run it on the **native** binary, render + evaluate the same way:
   - native satisfies `expect` → **BROWSER-GAP** (native works, browser
     doesn't → a real engine bug; **fails CI**).
   - native also fails `expect` → **BOTH-FAIL** (broken on native too — a
     guest/libc gap, e.g. the rune-locale `<_ctype.h>` hole; **does not fail
     CI**, it's a worklist entry).

CI fails **only on a fresh BROWSER-GAP**, so known both-wrong cases
(grep/sed/tr today) don't block the always-on browser-regression net.

### Documenting a pre-existing browser gap

A case may carry `"knownBrowserGap": "<reason>"`. When such a case classifies as
BROWSER-GAP it is still run and reported — under its own **KNOWN-BROWSER-GAP**
heading — but it does **not** gate CI. This is for a documented, pre-existing
engine gap (e.g. `sort` today: the engine models std streams as integer
sentinels, but the guest's `feof()` macro dereferences the FILE pointer). It
mirrors `parity_runner.mjs`'s `required:false` KNOWN-GAP entries: reported, not
hidden; a worklist item, not a CI blocker. A regression on any currently-passing
case still fails CI. When the engine gap is fixed, drop the flag and the case
becomes enforced.

Seed the correct expected output even for commands that are currently broken on
native — they land in BOTH-FAIL until the guest/libc gap is fixed, then flip to
PASS on both backends automatically, with no test edit.
