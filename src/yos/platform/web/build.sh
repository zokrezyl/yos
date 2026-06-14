#!/usr/bin/env bash
# Build the two wasm modules that share one linear memory:
#   guest.wasm   — the app, imports env.write / env.strlen
#   bridge.wasm  — the yos-host bridge, exports them, imports host.write_bytes
#
# Both import their linear memory (env.memory) instead of defining it,
# so the embedder owns ONE Memory and hands it to both. That is what
# makes a guest offset and a bridge offset the same address.
set -euo pipefail
cd "$(dirname "$0")"

CC=${YOS_WASM_CLANG:-clang}
# -fno-builtin so the guest's strlen stays a real env.strlen import
# (otherwise clang folds strlen-of-a-literal and the cross-module call
# vanishes, hiding the pure-wasm-bridge path we want to exercise).
COMMON=(-target wasm32-unknown-unknown -nostdlib -O2 -fno-builtin -Wl,--no-entry -Wl,--import-memory)

# Guest: tiny static footprint (one string near the bottom of memory).
"$CC" "${COMMON[@]}" \
	-Wl,--export=_start \
	-o guest.wasm guest.c

# Bridge: park its (near-empty) data + stack high above the guest's
# footprint so the two modules' static regions never overlap in the
# shared memory. This is the proof-slice stand-in for the real layout
# contract (yos-host private state vs. per-guest memory).
"$CC" "${COMMON[@]}" \
	-Wl,--global-base=8388608 \
	-Wl,-z,stack-size=65536 \
	-o bridge.wasm bridge.c

# Interactive shell stand-in guest (imports env.write; exports
# shell_start + feed_byte that the terminal drives per keystroke).
"$CC" "${COMMON[@]}" \
	-Wl,--export=shell_start -Wl,--export=feed_byte \
	-o shell_demo.wasm shell_demo.c

echo "built guest.wasm ($(wc -c <guest.wasm)) bridge.wasm ($(wc -c <bridge.wasm)) shell_demo.wasm ($(wc -c <shell_demo.wasm))"

# Real-threads demo: shared memory + atomics so worker threads address
# the same linear memory; exported function table so a worker can call a
# thread's start routine by pointer.
"$CC" -target wasm32-unknown-unknown -nostdlib -O2 -matomics -mbulk-memory \
	-Wl,--no-entry -Wl,--import-memory -Wl,--shared-memory \
	-Wl,--max-memory=67108864 -Wl,--export-table \
	-Wl,--export=run_racy -Wl,--export=run_locked -Wl,--export=thread_fn \
	-o threads_demo.wasm threads_demo.c
echo "built threads_demo.wasm ($(wc -c <threads_demo.wasm))"
