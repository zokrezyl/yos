"""find(1): walk filesystem tree.

Status: XFAIL today. Traps with `unresolved import env.fts_open`.
find's main loop is built around fts(3) (file-traversal stream).
Our auto-bridge skips fts.h entirely (extract.py doesn't include
it). The fix is to add an fts_* family bridge — they need a host
shadow state since fts walks lazily.
"""
import sys
from run_tool import get_paths, run


def main():
    yos, libexec = get_paths()
    r = run(yos, libexec, "find", ".", expect_rc=None, timeout=5)
    if r.returncode == 0 and len(r.stdout) > 0:
        print("PASS: find . produced output")
        sys.exit(0)
    if b"unresolved import env.fts_open" in r.stderr:
        print("XFAIL: find needs fts_* bridge family")
        sys.exit(1)
    print(f"FAIL: find unexpected: rc={r.returncode}")
    print(f"  stdout: {r.stdout!r}, stderr: {r.stderr!r}")
    sys.exit(1)


if __name__ == "__main__":
    main()
