"""ls(1): list directory contents.

Status: PASS. fts(3) is now wired in — FreeBSD's lib/libc/gen/fts.c
links straight into ls (libcExtras = [..., "fts", "qsort", "reallocf"])
and the host-side opendir/readdir/closedir/dirfd bridges live in
src/yos/impl/dir.c. Without that chain ls used to trap with
`env.fts_open` unresolved.

The check here is intentionally narrow: list /, expect to find at
least `bin` and `etc` (any sane sysroot has those). We don't pin
the exact entry set because /'s contents vary across hosts.
"""
import sys
from run_tool import get_paths, run


def main():
    yos, libexec = get_paths()
    r = run(yos, libexec, "ls", "/", expect_rc=0, timeout=10)
    names = set(r.stdout.decode(errors="replace").split())
    if "bin" in names and "etc" in names:
        print(f"PASS: ls / returned {len(names)} entries including bin/etc")
        sys.exit(0)
    print(f"FAIL: ls / missing bin or etc in output ({len(names)} names)")
    print(f"  stdout head: {r.stdout[:200]!r}")
    print(f"  stderr: {r.stderr!r}")
    sys.exit(1)


if __name__ == "__main__":
    main()
