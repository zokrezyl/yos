#!/usr/bin/env bash
# yos.sh — launch yos with sensible defaults.
#
#   ./tools/yos.sh                       interactive wasm zsh
#   ./tools/yos.sh -c 'echo hi'          one-shot zsh
#   ./tools/yos.sh nvim FILE             run a libexec tool directly
#
# Server mode (runit-driven supervisor):
#
#   ./tools/yos.sh --server [--log-dir DIR] [--daemon]
#
#   --server      Skip zsh and exec runsvdir on the service dir
#                 under <repo>/runtime/runit/. Each subdir is a
#                 runit service; runsvdir spawns one runsv per.
#                 Pass --server to the yos host too — reserved for
#                 future server-aware behaviour (today it's a
#                 no-op other than flag presence).
#   --daemon      Daemonize the yos host: fork+setsid+fork, chdir
#                 to /, redirect stdio to <log-dir>/yos-server.log,
#                 write PID to <log-dir>/yos-server.pid. Implies
#                 --server. Requires --log-dir.
#   --log-dir DIR
#                 Catch-all log directory. Default = <repo>/runtime/logs.
#                 The yos host writes its stderr there when --daemon
#                 is set; LOG_DIR=<dir> is exported into runsvdir's
#                 env so each service's log/run script can
#                 `exec svlogd $LOG_DIR/<svc>` for per-service
#                 rotating files.
#
# Builds .#all on first run (nix path-info is read-only — won't build).
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"

ALL="$(nix path-info .#all 2>/dev/null || true)"
if [ -z "$ALL" ] || [ ! -e "$ALL" ]; then
    nix build .#all >/dev/null
    ALL="$(nix path-info .#all)"
fi

# ── parse server-mode flags from the FRONT of argv ───────────────────
SERVER=0
DAEMON=0
LOG_DIR=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --server)  SERVER=1; shift ;;
        --daemon)  DAEMON=1; SERVER=1; shift ;;
        --log-dir) LOG_DIR="$2"; shift 2 ;;
        --log-dir=*) LOG_DIR="${1#--log-dir=}"; shift ;;
        *) break ;;
    esac
done

if [ "$SERVER" = 1 ]; then
    # Default log dir → repo/runtime/logs.
    [ -n "$LOG_DIR" ] || LOG_DIR="$REPO/runtime/logs"
    mkdir -p "$LOG_DIR"

    SVCDIR="$REPO/runtime/runit"
    if [ ! -d "$SVCDIR" ]; then
        echo "yos.sh --server: $SVCDIR does not exist" >&2
        exit 1
    fi

    # exec yos → runsvdir on the service dir. Pass --server / --daemon /
    # --log-dir to the yos binary so the daemonize dance happens HOST-
    # side (before wasm load) and so the LOG_DIR env var is set for
    # every forked service. PATH is the umbrella libexec/ so each
    # service's `run` can exec wasm tools by bare name.
    set -- "$ALL/bin/yos" \
           --server \
           $([ "$DAEMON" = 1 ] && echo --daemon) \
           --log-dir "$LOG_DIR" \
           "$ALL/libexec/runsvdir" -P "$SVCDIR"
    exec env PATH="$ALL/libexec" LOG_DIR="$LOG_DIR" "$@"
fi

# ── non-server mode (interactive / one-shot) ────────────────────────
# Three invocation shapes:
#   ./tools/yos.sh                   → interactive zsh
#   ./tools/yos.sh -c 'echo hi'      → forwarded to zsh -c
#   ./tools/yos.sh -<flag>           → forwarded to zsh
#   ./tools/yos.sh nvim file.txt     → exec libexec/nvim directly under yos
if [ "$#" -gt 0 ] && [ -e "$ALL/libexec/$1" ] && [ "${1#-}" = "$1" ]; then
    PROG="$1"; shift
    exec env PATH="$ALL/libexec" "$ALL/bin/yos" "$ALL/libexec/$PROG" "$@"
fi
exec env PATH="$ALL/libexec" "$ALL/bin/yos" "$ALL/libexec/zsh" "$@"
