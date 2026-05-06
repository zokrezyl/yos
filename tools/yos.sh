#!/usr/bin/env bash
# yos.sh — start zsh inside yos, forward all args.
#
#   ./tools/yos.sh                # interactive prompt
#   ./tools/yos.sh -c 'echo hi'   # one-shot
#
# Builds .#all on first run (nix path-info is read-only — won't build).
set -euo pipefail

cd "$(dirname "$0")/.."

ALL="$(nix path-info .#all 2>/dev/null || true)"
if [ -z "$ALL" ] || [ ! -e "$ALL" ]; then
    nix build .#all >/dev/null
    ALL="$(nix path-info .#all)"
fi

# PATH inside the wasm guest points at the umbrella's libexec/. That's
# where the bare .wasm executables live; yos's exec bridge can load
# them directly. The shims under bin/ are host-bash scripts and would
# be unreachable from inside the wasm sandbox (yos can't spawn host
# bash). Replacing the host PATH (rather than prepending) avoids the
# guest trying to exec /usr/bin/* and getting "unknown format" errors.

# Three invocation shapes:
#   ./tools/yos.sh                   → interactive zsh
#   ./tools/yos.sh -c 'echo hi'      → forwarded to zsh -c
#   ./tools/yos.sh -<flag>           → forwarded to zsh
#   ./tools/yos.sh nvim file.txt     → exec libexec/nvim directly under yos
# The third shape lets `./tools/yos.sh nvim` (or any other shipped
# wasm tool: cat, echo, …) launch the program directly without going
# through zsh's argv parsing — which would otherwise see "nvim" as a
# script-file path and fail with "no such file" since nvim isn't a
# shell script.
if [ "$#" -gt 0 ] && [ -e "$ALL/libexec/$1" ] && [ "${1#-}" = "$1" ]; then
    PROG="$1"; shift
    exec env PATH="$ALL/libexec" "$ALL/bin/yos" "$ALL/libexec/$PROG" "$@"
fi
exec env PATH="$ALL/libexec" "$ALL/bin/yos" "$ALL/libexec/zsh" "$@"
