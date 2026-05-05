{ stdenv, lib, binaryen, toolchain, sysroot, freebsd-src
, yos ? null   # see runner-script note below
}:

# yos-freebsd-tools — FreeBSD-base userland ported to wasm32, built
# in ONE Nix derivation against the shared FreeBSD source tree +
# yos sysroot + wasm toolchain.
#
# Why one derivation instead of one-per-tool?
#   - All tools share inputs (freebsd-src, sysroot, toolchain).
#     Per-tool derivations meant 17× setup overhead for every
#     evaluation + 17 separate /nix/store paths to manage.
#   - The user-facing artifact is naturally a single `bin/` directory
#     with N executables — exactly what one $out gives us.
#   - Adding a new tool now = one entry in the `tools` attrset below,
#     no new file, no flake.nix wiring.
#
# Output layout:
#   $out/bin/<name>          shell runner (`exec yos $out/libexec/<name> "$@"`)
#   $out/libexec/<name>      wasm module (no suffix)
#   $out/manifest.txt        per-tool metadata (pname/srcDir/srcs)
#
# To run a tool:
#     result/bin/cat /etc/hostname            # via the runner
#     yos $(nix path-info .#freebsd-tools)/libexec/cat /etc/hostname
#
# Adding a tool:
#   1. Find its sources under build-tools/freebsd/src/usr/src/{bin,usr.bin}/
#   2. Add an entry to `tools` below: pname → { srcDir, srcs?, libcExtras?, extraCflags? }.
#   3. `nix build .#freebsd-tools` — the single derivation rebuilds and
#      adds your tool to its $out/bin and $out/libexec. No flake change.

let
  # ── libc helpers ─────────────────────────────────────────────────────
  # Yos doesn't bridge every libc fn at the host surface; the simplest
  # answer for fns that are pure wasm-side (no kernel call) is to
  # compile FreeBSD's own libc source straight in. Tools opt in via
  # `libcExtras = [ "<name>" … ]` on their entry in `tools`.
  libcExtraTable = {
    getopt      = { sub = "lib/libc/stdlib"; file = "getopt.c"; };
    getopt_long = { sub = "lib/libc/stdlib"; file = "getopt_long.c"; };
    err         = { sub = "lib/libc/gen";    file = "err.c"; };
    basename    = { sub = "lib/libc/gen";    file = "basename.c"; };
    strsignal   = { sub = "lib/libc/string"; file = "strsignal.c"; };
  };

  # Default libc helpers every tool gets unless it opts out by setting
  # `libcExtras = []` (e.g. `yes`, `sync` — no flag parsing, no signals).
  defaultLibcExtras = [ "getopt" "getopt_long" "basename" "strsignal" ];

  # ── tool definitions ─────────────────────────────────────────────────
  # Each entry: pname → {
  #   srcDir       : path under usr/src/                        (required)
  #   srcs         : .c basenames in srcDir                     (default: ["<pname>.c"])
  #   libcExtras   : libc helpers from libcExtraTable           (default: defaultLibcExtras)
  #   extraSrcDirs : extra paths to search for srcs (FreeBSD .PATH style)
  #   extraCflags  : tool-specific -D / -I
  # }
  tools = {
    # ── FreeBSD bin/ ─────────────────────────────────────────────────
    cat      = { srcDir = "bin/cat";      libcExtras = [ "getopt" ]; extraCflags = [ "-DBOOTSTRAP_CAT" ]; };
    echo     = { srcDir = "bin/echo";     libcExtras = []; };
    pwd      = { srcDir = "bin/pwd";      libcExtras = [ "getopt" ]; };
    sleep    = { srcDir = "bin/sleep";    };
    sync     = { srcDir = "bin/sync";     libcExtras = []; };
    realpath = { srcDir = "bin/realpath"; };
    ln       = { srcDir = "bin/ln";       };
    rmdir    = { srcDir = "bin/rmdir";    };
    mkdir    = { srcDir = "bin/mkdir";    };
    hostname = { srcDir = "bin/hostname"; };
    test     = { srcDir = "bin/test";     };
    kill     = { srcDir = "bin/kill";     };

    # ── FreeBSD usr.bin/ ─────────────────────────────────────────────
    "true"   = { srcDir = "usr.bin/true";  libcExtras = []; };
    "false"  = { srcDir = "usr.bin/false"; libcExtras = []; };
    basename = { srcDir = "usr.bin/basename"; };
    dirname  = { srcDir = "usr.bin/dirname";  };
    head     = { srcDir = "usr.bin/head";     };
    uniq     = { srcDir = "usr.bin/uniq";     };
    yes      = { srcDir = "usr.bin/yes";      libcExtras = []; };
    id       = { srcDir = "usr.bin/id";       };
    mktemp   = { srcDir = "usr.bin/mktemp";   };
    touch    = { srcDir = "usr.bin/touch";    };
  };

  # ── helpers (Nix side) ───────────────────────────────────────────────
  toolSrcs       = t: t.srcs       or [ "${(t._name or "")}.c" ];
  toolExtras     = t: t.libcExtras or defaultLibcExtras;
  toolDirs       = t: t.extraSrcDirs or [];
  toolCflags     = t: t.extraCflags  or [];

  libcCflags = lib.optionals
    (lib.any (t: (toolExtras t) != []) (lib.attrValues tools))
    [ "-I${freebsd-src}/usr/src/lib/libc/include" ];

  # Render the bash buildPhase loop. Each tool becomes a build_tool call
  # with its arguments folded into a heredoc-friendly form.
  toolBuildScript = pname: cfg:
    let
      e         = cfg // { _name = pname; };
      srcs      = toolSrcs e;
      extras    = toolExtras e;
      dirs      = toolDirs e;
      cf        = toolCflags e;
      libcSrcs  = map (n:
        let lc = libcExtraTable.${n}; in
        "${freebsd-src}/usr/src/${lc.sub}/${lc.file}"
      ) extras;
      searchDirs = [ "${freebsd-src}/usr/src/${cfg.srcDir}" ]
                ++ map (d: "${freebsd-src}/usr/src/${d}") dirs;
    in ''
      build_tool ${pname} \
        "${cfg.srcDir}" \
        "${lib.concatStringsSep " " srcs}" \
        "${lib.concatStringsSep " " searchDirs}" \
        "${lib.concatStringsSep " " libcSrcs}" \
        "${lib.concatStringsSep " " (cf ++ libcCflags)}"
    '';

  yosRunner = wasmPath: pname:
    if yos != null
    then ''exec ${yos}/bin/yos ${wasmPath} "$@"''
    else ''exec yos ${wasmPath} "$@"'';
in

stdenv.mkDerivation {
  pname   = "yos-freebsd-tools";
  version = freebsd-src.version;

  dontUnpack    = true;
  dontConfigure = true;
  dontPatch     = true;
  dontStrip     = true;
  dontPatchELF  = true;
  dontFixup     = true;

  nativeBuildInputs = [ toolchain binaryen ];

  buildPhase = ''
    runHook preBuild

    set -euo pipefail
    mkdir -p out/bin out/libexec

    # ── compile_c <found.c> <out.o> ───────────────────────────────────
    # Single per-source compile — same flags whether the .c is a tool
    # source or a libc helper. Tool-specific cflags get appended by the
    # caller when needed.
    compile_c () {
      local found="$1" obj="$2" extra_cflags="$3"
      wasm-clang \
        -target wasm32-unknown-unknown -nostdlib -nostdinc \
        --sysroot="${sysroot}" \
        -isystem "${sysroot}/usr/include" \
        -D__i386__=1 -D__yos__=1 \
        -O2 -fno-builtin -ffreestanding \
        -Wno-unused-parameter -Wno-unused-but-set-variable \
        $extra_cflags \
        -c "$found" -o "$obj"
    }

    # ── build_tool <pname> <srcDir> "<srcs>" "<searchDirs>" "<libcSrcs>" "<cflags>" ──
    build_tool () {
      local pname="$1" srcDir="$2" srcs="$3" searchDirs="$4" libcSrcs="$5" cflags="$6"
      local objs=()
      local td="$(mktemp -d)"

      # Tool srcs — search through searchDirs (mimics FreeBSD .PATH).
      for s in $srcs; do
        local found=""
        for d in $searchDirs; do
          if [ -f "$d/$s" ]; then found="$d/$s"; break; fi
        done
        if [ -z "$found" ]; then
          echo "freebsd-tools($pname): src '$s' not found under: $searchDirs" >&2
          exit 1
        fi
        local obj="$td/tool_$(basename "$s" .c).o"
        compile_c "$found" "$obj" "$cflags"
        objs+=("$obj")
      done

      # libc helpers — absolute paths, uniquified obj names so a tool
      # whose own source matches a libc helper (basename/strsignal/…)
      # doesn't get duplicate symbols at link time.
      for p in $libcSrcs; do
        local obj="$td/libc_$(basename "$p" .c).o"
        compile_c "$p" "$obj" "$cflags"
        objs+=("$obj")
      done

      # Link.
      wasm-clang \
        -target wasm32-unknown-unknown -nostdlib \
        --sysroot="${sysroot}" \
        -L"${sysroot}/usr/lib" \
        -Wl,--no-entry -Wl,--allow-undefined -Wl,--export=_start \
        -Wl,--export=main \
        -o "out/libexec/$pname" \
        "${sysroot}/usr/lib/crt1.o" \
        "''${objs[@]}" \
        -lc -lyos_stubs

      # Runner — `exec yos <libexec-path> "$@"`. yos store path is
      # baked in when the .#yos derivation is wired (yos != null);
      # otherwise we fall back to looking up `yos` on PATH.
      cat > "out/bin/$pname" <<RUNNER_EOF
    #!/usr/bin/env bash
    exec ${if yos != null then "${yos}/bin/yos" else "yos"} "$out/libexec/$pname" "\$@"
    RUNNER_EOF
      chmod +x "out/bin/$pname"

      rm -rf "$td"
    }

    # ── run the build for each tool ───────────────────────────────────
    ${lib.concatStrings (lib.mapAttrsToList toolBuildScript tools)}

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out
    cp -r out/bin     $out/bin
    cp -r out/libexec $out/libexec

    # Per-tool metadata for diffability against the upstream Makefiles.
    {
      echo "yos-freebsd-tools ${freebsd-src.version}"
      echo
      ${lib.concatStrings (lib.mapAttrsToList (pname: cfg: ''
        echo "  ${pname}    src=usr/src/${cfg.srcDir}    srcs=${
          lib.concatStringsSep " " (cfg.srcs or [ "${pname}.c" ])
        }"
      '') tools)}
    } > $out/manifest.txt

    runHook postInstall
  '';

  meta = with lib; {
    description = "FreeBSD-base userland (cat, echo, ls, …) ported to wasm32 / yos";
    license     = licenses.bsd2;
    platforms   = platforms.linux ++ platforms.darwin;
  };

  passthru = {
    # The list of tools we provide — useful for downstream code that
    # wants to enumerate them without re-parsing the derivation.
    toolNames = lib.attrNames tools;
  };
}
