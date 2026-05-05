"""Shared helper to locate the wasm-built zsh binary.

Discovery is read-only — never triggers a nix build (that could take
minutes and would blow past meson's 30s timeout). If the artifact
isn't already realized somewhere we know about, return None and let
the caller SKIP.

Lookup order:
  1. YOS_ZSH_WASM env var (absolute path to .wasm)
  2. result*/bin/zsh.wasm — symlinks left by a manual `nix build .#zsh`
  3. `nix path-info .#zsh` — uses *only* if the path already exists
     (no realization). Cheap eval: ~100ms.
"""
import os
import subprocess
import glob


def find_zsh_wasm(repo: str) -> str | None:
    p = os.environ.get("YOS_ZSH_WASM")
    if p and os.path.exists(p):
        return p

    for sym in sorted(glob.glob(os.path.join(repo, "result*"))):
        cand = os.path.join(sym, "bin", "zsh.wasm")
        if os.path.exists(cand):
            return cand

    try:
        out = subprocess.run(
            ["nix", "path-info", ".#zsh"],
            cwd=repo, capture_output=True, text=True, timeout=10,
        )
        if out.returncode == 0:
            cand = os.path.join(out.stdout.strip(), "bin", "zsh.wasm")
            if os.path.exists(cand):
                return cand
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    return None
