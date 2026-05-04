"""nvim quit-cleanly test: send :q!, check it exits with status 0.

Regression for the EVFILT_PROC|NOTE_EXIT shutdown path: libuv on
__FreeBSD__ uses kqueue PROC events (not SIGCHLD) to detect that the
embedded server child has exited. Without yos's PROC delivery + the
4-byte rwlock fix that this depends on, the parent TUI would either
hang waiting for a kevent that never fires or trap with
out-of-bounds memory access in uv__io_poll.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_in_pty import run_in_pty


def main():
    repo = os.environ.get("YOS_REPO_ROOT") or os.getcwd()
    # YOS_BUILD_DIR overrides where the host yos binary lives; defaults
    # to build-linux for back-compat with the original linux invocation.
    # The wasm-pkgs path stays under build-linux/ regardless of host —
    # see tools/wasm-pkg.sh for the rationale.
    build_dir = os.environ.get("YOS_BUILD_DIR") or "build-linux"
    yos = os.path.join(repo, build_dir, "src", "yos", "yos")
    nvim = os.path.join(repo, "build-linux", "wasm-pkgs", "nvim-0.10.4",
                        "out", "bin", "nvim.wasm")
    if not os.path.exists(yos):
        print(f"FAIL: yos binary not found: {yos}")
        sys.exit(1)
    if not os.path.exists(nvim):
        print(f"FAIL: nvim.wasm not found: {nvim}")
        sys.exit(1)
    env = dict(os.environ)
    env["TERM"] = "xterm-256color"
    out, status = run_in_pty(
        [yos, nvim],
        rows=24, cols=80,
        driver=[(2.5, b""), (0.5, b":q!\r"), (3.0, b"")],
        kill_after=2.0,
        env=env,
    )
    if status != 0:
        print(f"FAIL: expected exit 0, got status={status}")
        sys.stdout.buffer.write(b"--- last 400 bytes ---\n")
        sys.stdout.buffer.write(out[-400:])
        sys.exit(1)
    print("PASS: nvim quit cleanly")


if __name__ == "__main__":
    main()
