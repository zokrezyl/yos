{ pkgs, src }:

# Top of the yos Nix tree. Foundation is layered:
#
#   1. yos        — host runtime binary (the wasm3-based loader, meson
#                   build). Lowest layer; depends on nothing yos-specific.
#   2. sysroot    — wasm32 sysroot built from the FreeBSD i386 headers
#                   (downloaded + extracted by build-tools/freebsd/) plus
#                   the yos-flavoured crt1 from build-tools/sysroot/skel.sh.
#   3. toolchain  — clang-unwrapped + wasm-ld + wasm-opt + the wasm-clang
#                   shim that filters darwin-only flags.
#   4. lib.buildRecipe — generic builder. Drives a recipe under
#                   build-tools/wasm-pkg/configs/<name>/build.sh with the
#                   same env (ROOT/PREFIX/WORK/WASM_CC/WASM_SYSROOT/
#                   WASM_CFLAGS/WASM_LDFLAGS/DEP_PREFIXES) that
#                   tools/wasm-pkg.sh provides.
#   5. wasm packages — nvim and its 9 dep libraries (lua, libuv,
#                   msgpack-c, unibilium, libvterm, tree-sitter, lpeg,
#                   lua-mpack, luv).
#
# Each layer's source is the yos repo itself (`src` arg) — no
# vendoring, no separate fetch. Move/rename a source file in the repo
# and these derivations pick it up.

let
  freebsd-src = pkgs.callPackage ./freebsd-src { };
  yos         = pkgs.callPackage ./yos        { inherit src; };
  sysroot     = pkgs.callPackage ./sysroot    { inherit src freebsd-src; };
  toolchain   = pkgs.callPackage ./toolchain  { inherit src; };

  buildRecipe = pkgs.callPackage ./lib/build-recipe.nix {
    inherit toolchain sysroot src yos;
  };

  buildFreebsdTool = pkgs.callPackage ./lib/build-freebsd-tool.nix {
    inherit toolchain sysroot freebsd-src;
    # yos = yos;   # uncomment once .#yos builds reliably; for now
    # the runner script falls back to `yos` on PATH so the tool
    # ports can land independently of the host derivation work.
    yos = null;
  };

  # Leaf wasm libraries (no inter-package deps).
  lua          = pkgs.callPackage ./pkgs/lua          { inherit buildRecipe; };
  libuv        = pkgs.callPackage ./pkgs/libuv        { inherit buildRecipe; };
  msgpack-c    = pkgs.callPackage ./pkgs/msgpack-c    { inherit buildRecipe; };
  unibilium    = pkgs.callPackage ./pkgs/unibilium    { inherit buildRecipe; };
  tree-sitter  = pkgs.callPackage ./pkgs/tree-sitter  { inherit buildRecipe; };
  libvterm     = pkgs.callPackage ./pkgs/libvterm     { inherit buildRecipe; };

  # Lua-side bindings (depend on lua / libuv).
  lpeg         = pkgs.callPackage ./pkgs/lpeg         { inherit buildRecipe lua; };
  lua-mpack    = pkgs.callPackage ./pkgs/lua-mpack    { inherit buildRecipe lua; };
  luv          = pkgs.callPackage ./pkgs/luv          { inherit buildRecipe lua libuv; };

  # The big consumer. callPackage's auto-resolution would otherwise pull
  # in nixpkgs's host-side `lua`/`libuv`/etc.; pass ours explicitly.
  nvim         = pkgs.callPackage ./pkgs/nvim {
    inherit buildRecipe sysroot
            lua libuv msgpack-c unibilium libvterm
            tree-sitter lpeg lua-mpack luv;
  };

  # FreeBSD-base userland — single derivation that compiles the whole
  # toolset (cat, echo, ls, sh, …) from the shared FreeBSD source tree
  # against our sysroot. Output: $out/bin/<name> + $out/libexec/<name>
  # for each tool. See pkgs/freebsd-tools/default.nix to add one.
  freebsd-tools = pkgs.callPackage ./pkgs/freebsd-tools {
    inherit toolchain sysroot freebsd-src yos;
  };

  # Upstream-tarball ports (autoconf, cmake, …) — wasm32 cross-builds
  # via the standard buildRecipe pattern. Each carries its own
  # build-tools/wasm-pkg/configs/<name>/build.sh recipe.
  zsh = pkgs.callPackage ./pkgs/zsh { inherit buildRecipe; };

  # Umbrella package: every user-facing yos artefact merged into one
  # tree via symlinkJoin. Lets users do
  #   nix run .#                    # drops into wasm zsh under yos (sandbox)
  #   nix shell .#all              # host shell with yos's bin/ on PATH
  #   nix build .#all              # one $out with yos + zsh + nvim + tools
  # without listing individual derivations. New top-level packages
  # should be added to `paths` below to keep this a single landing zone.
  #
  # `bin/yos-shell` is the security-relevant entry point: it strips the
  # host's environment via `env -i`, sets PATH to `$out/libexec/` (raw
  # wasm modules — yos's exec bridge can load these directly), and execs
  # yos→zsh.wasm. From inside that shell every command resolves through
  # yos's wasm process model; nothing reaches host /usr/bin. That's the
  # sandbox boundary.
  all = pkgs.symlinkJoin {
    name = "yos-all";
    paths = [ yos zsh nvim freebsd-tools ];
    postBuild = ''
      cat > $out/bin/yos-shell <<RUNNER_EOF
      #!/usr/bin/env bash
      # yos-shell — pristine wasm-zsh sandbox under yos.
      # PATH points at the umbrella's libexec/ where the bare .wasm
      # modules live. The runner shims in bin/ are host-bash scripts;
      # we don't put them on PATH because yos can't load shebang-bash
      # from inside the wasm guest. Keeping libexec on PATH means
      # lookups inside the wasm guest resolve to wasm modules yos can
      # exec directly. Anything not under $out/libexec is unreachable
      # from inside this shell — that's the sandbox boundary.
      # No args → interactive prompt. Args → forwarded to zsh, so
      # \`nix run .# -- -c 'echo hi'\` works as a one-shot.
      if [ "\$#" -eq 0 ]; then
          set -- -i
      fi
      exec env -i \\
          HOME="\$HOME" \\
          USER="\''${USER:-yos}" \\
          TERM="\''${TERM:-xterm-256color}" \\
          PATH="$out/libexec" \\
          "$out/bin/yos" "$out/libexec/zsh" "\$@"
      RUNNER_EOF
      chmod +x $out/bin/yos-shell
    '';
    meta = with pkgs.lib; {
      description = "yos host runtime + every wasm32 port (zsh, nvim, freebsd-tools) merged into one bin/libexec tree, plus yos-shell sandbox wrapper";
      platforms   = platforms.linux ++ platforms.darwin;
    };
  };
in {
  inherit yos sysroot toolchain freebsd-src buildRecipe buildFreebsdTool
          lua libuv msgpack-c unibilium tree-sitter libvterm
          lpeg lua-mpack luv nvim
          freebsd-tools
          zsh
          all;
}
