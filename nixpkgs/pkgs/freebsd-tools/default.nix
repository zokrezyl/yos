{ stdenv, lib, binaryen, bison, buildPackages, toolchain, sysroot, freebsd-src
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
#   2. Add an entry to `tools` below: pname → { srcDir, srcs?, libcExtras?,
#      extraSrcDirs?, stageDirs?, extraCflags? }.
#   3. `nix build .#freebsd-tools` — the single derivation rebuilds and
#      adds your tool to its $out/bin and $out/libexec. No flake change.
#
# Schema notes (per-tool fields):
#   srcDir       — path under usr/src/ that holds the tool's sources    (required)
#   srcs         — .c basenames; default ["<pname>.c"]
#   libcExtras   — libc helper sources from libcExtraTable; default
#                  defaultLibcExtras (getopt/getopt_long/basename/strsignal)
#   extraSrcDirs — additional usr/src/<dir> paths searched for srcs
#                  (FreeBSD .PATH semantics)
#   stageDirs    — additional `$STAGE/<dir>` paths searched for srcs;
#                  used for files generated at build time (bison output,
#                  maketab output, libxo shim) staged before build_tool
#                  runs
#   extraCflags  — tool-specific -D / -I; may reference $STAGE
#
# Ports that are deliberately NOT in this derivation:
#   ps, top      — need libkvm and a kernel-style proc table; yos has a
#                  host-side proc table instead, so they'd need a
#                  reimplemented machine.c reading from yos's procfs.
#   tar          — needs libarchive (~500 source files); belongs in its
#                  own derivation alongside libarchive itself.
#   gzip         — needs libz, liblzma, libzstd (and optionally libbz2);
#                  each is its own wasm port + derivation.

let
  # ── libc helpers ─────────────────────────────────────────────────────
  # Yos doesn't bridge every libc fn at the host surface; the simplest
  # answer for fns that are pure wasm-side (no kernel call) is to
  # compile FreeBSD's own libc source straight in. Tools opt in via
  # `libcExtras = [ "<name>" … ]` on their entry in `tools`.
  libcExtraTable = {
    getopt       = { sub = "lib/libc/stdlib"; file = "getopt.c"; };
    getopt_long  = { sub = "lib/libc/stdlib"; file = "getopt_long.c"; };
    err          = { sub = "lib/libc/gen";    file = "err.c"; };
    basename     = { sub = "lib/libc/gen";    file = "basename.c"; };
    strsignal    = { sub = "lib/libc/string"; file = "strsignal.c"; };
    # fts(3) — directory-tree walker used by ls, cp, find, du, rm -r,
    # chmod -R, etc. Pure userspace traversal sitting on top of
    # opendir/readdir/closedir/fstatfs/fchdir/lstat — all already
    # bridged in yos. Pulled verbatim from FreeBSD libc; the FreeBSD-
    # internal headers it needs (namespace.h, un-namespace.h,
    # gen-private.h, libc_private.h) come from -I lib/libc/include
    # and lib/libc/gen (added in libcCflags below).
    #
    # fts.c references the underscored symbols _open/_close/_fstat/
    # _fstatfs and __opendir2 — these are FreeBSD libc-internal aliases
    # for the public POSIX calls. The sysroot's libyos_stubs.a provides
    # thin wrappers so they resolve at link time.
    fts          = { sub = "lib/libc/gen";    file = "fts.c"; };
    # qsort(3) — fts(3) uses it to sort directory entries when the
    # user passes a comparator to fts_open(). Compiling FreeBSD's
    # qsort.c into the tool means the comparator callback stays a
    # wasm function pointer (no cross-runtime call) and we get
    # FreeBSD-faithful ordering semantics.
    qsort        = { sub = "lib/libc/stdlib"; file = "qsort.c"; };
    # reallocf(3) — realloc that frees the old pointer on failure.
    # FreeBSD-specific; pulled in for tools that link fts (fts.c uses
    # it during path-buffer growth).
    reallocf     = { sub = "lib/libc/stdlib"; file = "reallocf.c"; };
  };

  # Default libc helpers every tool gets unless it opts out by setting
  # `libcExtras = []` (e.g. `yes`, `sync` — no flag parsing, no signals).
  defaultLibcExtras = [ "getopt" "getopt_long" "basename" "strsignal" ];

  # Tools that touch ctype (isalpha, isalnum, tolower, …) or call
  # setlocale(LC_CTYPE, …) — i.e. anything that processes text — need
  # _CurrentRuneLocale set to a rune table. Without it every is*()
  # returns 0: awk reads only the first byte of any keyword, tr never
  # matches a-z, etc. We can't compile FreeBSD's lib/libc/locale/table.c
  # straight to wasm32 — it pulls in machine/atomic.h with x86 asm.
  # The `yos_locale_stub.c` in stage-dir gives us a static C-locale
  # rune table (ASCII flags + lower/upper maps) and the global decls.
  ctypeLibcExtras = defaultLibcExtras;
  # The locale stub is staged in $STAGE/locale-stub by buildPhase.

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
    # sh     = { srcDir = "bin/sh";     };
    #ps     = { srcDir = "bin/ps";     };
    cat      = { srcDir = "bin/cat";      libcExtras = [ "getopt" ]; extraCflags = [ "-DBOOTSTRAP_CAT" ]; };
    cp       = { srcDir = "bin/cp";       srcs = [ "cp.c" "utils.c" ];
                 libcExtras  = defaultLibcExtras ++ [ "fts" "qsort" "reallocf" ];
                 extraCflags = [ "-D_ACL_PRIVATE" ]; };
    chmod    = { srcDir = "bin/chmod";    };
    date     = { srcDir = "bin/date";     srcs = [ "date.c" "vary.c" ]; };
    dd       = { srcDir = "bin/dd";       srcs = [ "args.c" "conv.c" "conv_tab.c" "dd.c" "misc.c" "position.c" ]; };
    echo     = { srcDir = "bin/echo";     libcExtras = []; };
    domainname = { srcDir = "bin/domainname"; };
    hostname = { srcDir = "bin/hostname"; };
    kill     = { srcDir = "bin/kill";     };
    ln       = { srcDir = "bin/ln";       };
    ls       = { srcDir = "bin/ls";       srcs = [ "cmp.c" "ls.c" "print.c" "util.c" "yos_locale_stub.c" ];
                 libcExtras = defaultLibcExtras ++ [ "fts" "qsort" "reallocf" ];
                 # print.c filters every output byte through isprint().
                 # Without the locale stub, _CurrentRuneLocale is NULL
                 # so isprint() returns 0 for every byte and ls (in
                 # default `-q` mode under a TTY) substitutes '?' for
                 # every character of every filename.
                 stageDirs = [ "locale-stub" ]; };
    mkdir    = { srcDir = "bin/mkdir";    };
    mv       = { srcDir = "bin/mv";       };
    pwd      = { srcDir = "bin/pwd";      libcExtras = [ "getopt" ]; };
    realpath = { srcDir = "bin/realpath"; };
    rm       = { srcDir = "bin/rm";       };
    rmdir    = { srcDir = "bin/rmdir";    };
    sleep    = { srcDir = "bin/sleep";    };
    stty     = { srcDir = "bin/stty";     srcs = [ "cchar.c" "gfmt.c" "key.c" "modes.c" "print.c" "stty.c" "util.c" ]; };
    sync     = { srcDir = "bin/sync";     libcExtras = []; };
    test     = { srcDir = "bin/test";     };
    timeout  = { srcDir = "bin/timeout";  };
    uuidgen  = { srcDir = "bin/uuidgen";  extraCflags = [ "-include" "errno.h" ]; };

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

    # ── second batch: pure libc, multi-source ────────────────────────
    # All of these process text and so include yos_locale_stub.c, which
    # provides _CurrentRuneLocale + __mb_sb_limit so isalpha/isalnum
    # actually work in the wasm sandbox.
    cut      = { srcDir = "usr.bin/cut"; srcs = [ "cut.c" "yos_locale_stub.c" ];
                 stageDirs = [ "locale-stub" ]; };
    xargs    = { srcDir = "usr.bin/xargs";
                 srcs = [ "xargs.c" "strnsubst.c" "yos_locale_stub.c" ];
                 stageDirs = [ "locale-stub" ]; };
    tr       = { srcDir = "usr.bin/tr";
                 srcs = [ "cmap.c" "cset.c" "str.c" "tr.c" "yos_locale_stub.c" ];
                 stageDirs = [ "locale-stub" ]; };
    sed      = { srcDir = "usr.bin/sed";
                 srcs = [ "compile.c" "main.c" "misc.c" "process.c" "yos_locale_stub.c" ];
                 stageDirs = [ "locale-stub" ]; };
    du       = { srcDir = "usr.bin/du"; srcs = [ "du.c" "yos_locale_stub.c" ];
                 libcExtras = ctypeLibcExtras ++ [ "fts" "qsort" "reallocf" ];
                 stageDirs = [ "locale-stub" ]; };
    grep     = { srcDir = "usr.bin/grep";
                 srcs = [ "file.c" "grep.c" "queue.c" "util.c" "yos_locale_stub.c" ];
                 stageDirs = [ "locale-stub" ]; };

    # ── third batch: extra source dirs / generated files ─────────────
    # find: getdate.y → getdate.c via bison; the codegen step in
    # buildPhase writes it into $STAGE/find-gen so we point a stageDir
    # at that.
    find     = { srcDir = "usr.bin/find";
                 srcs = [ "find.c" "function.c" "ls.c" "main.c" "misc.c"
                          "operator.c" "option.c" "getdate.c"
                          "yos_locale_stub.c" ];
                 libcExtras = defaultLibcExtras ++ [ "fts" "qsort" "reallocf" ];
                 stageDirs   = [ "find-gen" "locale-stub" ];
                 extraCflags = [ "-Wno-incompatible-pointer-types" ]; };
    # sort: pulls md5c.c straight in from lib/libmd, then -Is the same
    # dir for <md5.h>.
    sort     = { srcDir = "usr.bin/sort";
                 srcs = [ "bwstring.c" "coll.c" "file.c" "mem.c" "radixsort.c"
                          "sort.c" "vsort.c" "md5c.c" "yos_locale_stub.c" ];
                 extraSrcDirs = [ "lib/libmd" ];
                 stageDirs    = [ "locale-stub" ];
                 extraCflags  = [ "-I${freebsd-src}/usr/src/lib/libmd"
                                  "-Wno-pointer-sign" ]; };
    # awk: bison generates awkgram.c + awkgram.tab.h; maketab compiled
    # for the host emits proctab.c. Both staged in $STAGE/awk-gen.
    awk      = { srcDir = "usr.bin/awk";
                 srcs = [ "awkgram.c" "b.c" "lex.c" "lib.c" "main.c"
                          "parse.c" "proctab.c" "run.c" "tran.c"
                          "yos_locale_stub.c" ];
                 extraSrcDirs = [ "contrib/one-true-awk" ];
                 stageDirs    = [ "awk-gen" "locale-stub" ];
                 extraCflags  = [ "-DHAS_ISBLANK" "-DFOPEN_MAX=64"
                                  "-I${freebsd-src}/usr/src/contrib/one-true-awk"
                                  "-I$STAGE/awk-gen"
                                  "-Wno-implicit-int" ]; };
    # wc, df: pull in our minimal text-only libxo shim staged at
    # $STAGE/libxo (yos_libxo_shim.c + libxo.h). Real libxo (8500+
    # lines, JSON / XML / HTML output) is overkill for wasm — we only
    # need text mode, which is ~150 lines of format parsing.
    wc       = { srcDir = "usr.bin/wc";
                 srcs = [ "wc.c" "yos_libxo_shim.c" "yos_locale_stub.c" ];
                 stageDirs   = [ "libxo" "locale-stub" ];
                 extraCflags = [ "-I$STAGE/libxo" "-Wno-pointer-sign" ]; };
    df       = { srcDir = "bin/df";
                 srcs = [ "df.c" "yos_libxo_shim.c" "yos_locale_stub.c" ];
                 stageDirs   = [ "libxo" "locale-stub" ];
                 extraCflags = [ "-I$STAGE/libxo" ]; };

    # ── yos-native ps ────────────────────────────────────────────────
    # FreeBSD's bin/ps is ~3000 lines and pulls in libkvm; libkvm
    # itself reads /dev/kmem and a kernel proc table that yos doesn't
    # have. yos exposes its process state through /proc (synthesised
    # by src/yos/vfs/procfs.c, mounted at startup in src/yos/main.c),
    # so the user-facing fix is a small ps that reads /proc directly.
    # Source is staged in $STAGE/yos-ps from buildPhase below.
    ps       = { srcDir = "bin"; /* unused — all srcs come from stageDirs */
                 srcs = [ "yos_ps.c" ];
                 stageDirs = [ "yos-ps" ];
                 libcExtras = [ ]; };
  };

  # ── helpers (Nix side) ───────────────────────────────────────────────
  toolSrcs       = t: t.srcs       or [ "${(t._name or "")}.c" ];
  toolExtras     = t: t.libcExtras or defaultLibcExtras;
  toolDirs       = t: t.extraSrcDirs or [];
  toolStageDirs  = t: t.stageDirs    or [];
  toolCflags     = t: t.extraCflags  or [];

  libcCflags = lib.optionals
    (lib.any (t: (toolExtras t) != []) (lib.attrValues tools))
    [ "-I${freebsd-src}/usr/src/lib/libc/include"
      # lib/libc/gen carries gen-private.h, which fts.c includes via
      # double-quote ("gen-private.h"). The compiler finds it in the
      # source file's own directory when fts.c is compiled, but the
      # consumer translation units (ls.c, cp.c, …) don't compile
      # anything from lib/libc/gen, so adding the dir to the search
      # path keeps the include resolvable from any TU that ends up
      # parsing it (none today, but cheap insurance).
      "-I${freebsd-src}/usr/src/lib/libc/gen" ];

  # Render the bash buildPhase loop. Each tool becomes a build_tool call
  # with its arguments folded into a heredoc-friendly form.
  #
  # Stage dirs are emitted as literal `$STAGE/<name>` strings — the
  # buildPhase pre-generates files there before any tool builds, so by
  # the time build_tool runs, $STAGE is set and the path resolves.
  toolBuildScript = pname: cfg:
    let
      e         = cfg // { _name = pname; };
      srcs      = toolSrcs e;
      extras    = toolExtras e;
      dirs      = toolDirs e;
      stageDirs = toolStageDirs e;
      cf        = toolCflags e;
      libcSrcs  = map (n:
        let lc = libcExtraTable.${n}; in
        "${freebsd-src}/usr/src/${lc.sub}/${lc.file}"
      ) extras;
      searchDirs = [ "${freebsd-src}/usr/src/${cfg.srcDir}" ]
                ++ map (d: "${freebsd-src}/usr/src/${d}") dirs
                ++ map (d: "$STAGE/${d}") stageDirs;
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

  nativeBuildInputs = [
    toolchain binaryen
    # bison: generates getdate.c (find), awkgram.c (awk).
    bison
    # host cc: builds awk's maketab tool that emits proctab.c.
    buildPackages.stdenv.cc
  ];

  buildPhase = ''
    runHook preBuild

    set -euo pipefail
    mkdir -p out/bin out/libexec

    # ── stub headers for compression libs we don't link ──────────────
    # FreeBSD's grep.h (and a few other ports) `#include` <bzlib.h> and
    # <zlib.h> for declarations they don't actually use — they're left
    # over from the original NetBSD zgrep wrapper. We don't ship any
    # of those compression libraries in the wasm sysroot, so provide
    # empty stubs in a tool-private include dir. Same trick for lzma /
    # zstd. Tools that actually need them (gzip) would never resolve
    # against these — they'd need a real port.
    STUBINC="$TMPDIR/stubinc"
    mkdir -p "$STUBINC"
    : > "$STUBINC/bzlib.h"
    : > "$STUBINC/zlib.h"
    : > "$STUBINC/lzma.h"
    : > "$STUBINC/zstd.h"

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
        -idirafter "$STUBINC" \
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

    # ── codegen for tools that need yacc/maketab ─────────────────────
    # Done up-front, in one place, so the per-tool build_tool calls
    # below can find generated .c / .h via stageDirs (rendered as
    # $STAGE/<name> in the searchDirs list).
    STAGE="$TMPDIR/yos-stage"
    mkdir -p "$STAGE"

    # find: getdate.y → getdate.c (yacc grammar). Bison's `-y` mode is
    # POSIX-yacc-compatible; FreeBSD uses byacc but the grammar is
    # plain enough that bison-as-yacc handles it. The grammar emits
    # the function `get_date()` that find/function.c calls into.
    mkdir -p "$STAGE/find-gen"
    bison -y -o "$STAGE/find-gen/getdate.c" \
        "${freebsd-src}/usr/src/usr.bin/find/getdate.y"

    # awk: bison generates awkgram.{c,h}; we then build a host-side
    # `maketab` binary and run it on awkgram.tab.h to emit proctab.c.
    # FreeBSD's Makefile renames awkgram.h → awkgram.tab.h with a
    # symlink; we just produce the .tab.h directly.
    AWK_SRC="${freebsd-src}/usr/src/contrib/one-true-awk"
    mkdir -p "$STAGE/awk-gen"
    bison --header="$STAGE/awk-gen/awkgram.tab.h" \
          -o "$STAGE/awk-gen/awkgram.c" \
          "$AWK_SRC/awkgram.y"
    # maketab is host-side build-tool (it runs at build time, not in
    # the wasm guest), so use the build-platform cc, not wasm-clang.
    "${buildPackages.stdenv.cc}/bin/cc" \
        -O2 -I"$STAGE/awk-gen" \
        -o "$STAGE/awk-gen/maketab" \
        "$AWK_SRC/maketab.c"
    "$STAGE/awk-gen/maketab" "$STAGE/awk-gen/awkgram.tab.h" \
        > "$STAGE/awk-gen/proctab.c"

    # ── locale stub: minimal C-locale rune table + __mb_sb_limit ─────
    # Without _CurrentRuneLocale set, every isalpha/isalnum/isspace
    # returns 0 (the inline __sbmaskrune in <_ctype.h> deref's a NULL
    # pointer's runetype[] field). awk reads only "B" before bailing,
    # tr never matches a-z, sed/grep can't lex regex classes.
    #
    # Compiling FreeBSD's lib/libc/locale/table.c straight to wasm32
    # fails: it includes mblocal.h → xlocale_private.h → atomic.h
    # which is x86 inline asm. Strip those includes + the host-side
    # __runes_for_locale function (only the static initializer is
    # needed for the C-locale fast path) and write the result + the
    # __mb_sb_limit definition into the stage dir.
    mkdir -p "$STAGE/locale-stub"
    LOC="${freebsd-src}/usr/src/lib/libc/locale"
    {
        # Skip mblocal.h include → no xlocale_private.h → no atomic.h
        sed -e 's|#include "mblocal.h"||' "$LOC/table.c" \
            | sed -e '/^_RuneLocale \*$/,/^}$/d' \
                  -e '/^__runes_for_locale/,/^}$/d'
        echo
        echo "/* yos addition: __mb_sb_limit lives in none.c upstream;"
        echo " * pulled out separately so we don't have to compile in"
        echo " * none.c (which calls into setlocale machinery). 256 is"
        echo " * the upstream none.c value (every byte is single-byte)."
        echo " */"
        echo "int __mb_sb_limit = 256;"
        echo ""
        echo "/* yos addition: <runetype.h> declares _ThreadRuneLocale as"
        echo " * _Thread_local and the inline __getCurrentRuneLocale reads"
        echo " * it on every is*()/iswXXX() call. Without a definition the"
        echo " * wasm-ld link only resolves it through --allow-undefined,"
        echo " * which leaves the TLS slot uninitialised; reads return"
        echo " * garbage pointers and __maskrune dereferences them. Define"
        echo " * it explicitly as the zero TLS so __getCurrentRuneLocale"
        echo " * falls through to the global _CurrentRuneLocale set above."
        echo " */"
        echo "_Thread_local const _RuneLocale *_ThreadRuneLocale = 0;"
    } > "$STAGE/locale-stub/yos_locale_stub.c"

    # ── yos-native ps ────────────────────────────────────────────────
    # Reads yos's synthetic /proc (mounted at startup by src/yos/main.c
    # via src/yos/vfs/procfs.c). Output mirrors `ps -e` from BSD ps
    # closely enough to be useful in the yos shell without dragging in
    # libkvm + 3 kloc of FreeBSD's actual bin/ps. Pure libc — opendir/
    # readdir/closedir + open/read on /proc/<pid>/stat.
    mkdir -p "$STAGE/yos-ps"
    cat > "$STAGE/yos-ps/yos_ps.c" <<'YOSPS_EOF'
    /* yos-native ps(1) — reads yos's synthetic /proc.
     *
     * Output columns: PID  PPID  STAT  COMMAND
     *
     * The /proc/<pid>/stat format is Linux-flavoured (see
     * src/yos/vfs/procfs.c generate_stat): "<pid> (<comm>) <state>
     * <ppid> <pgrp> <session> 0 -1 0 ...". We parse just the first
     * four fields and ignore the rest.
     */
    #include <ctype.h>
    #include <dirent.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <unistd.h>

    static int is_all_digits(const char *s)
    {
        if (!*s) return 0;
        for (; *s; s++) if (*s < '0' || *s > '9') return 0;
        return 1;
    }

    /* Parse /proc/<pid>/stat. comm comes back in `comm_out`
     * (NUL-terminated, up to `comm_max` bytes including the NUL).
     * Returns 0 on success, -1 on parse failure. */
    static int parse_stat(const char *path, int *pid, int *ppid,
                          char *state, char *comm_out, size_t comm_max)
    {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return -1;
        buf[n] = '\0';

        /* pid */
        char *p = buf;
        *pid = (int)strtol(p, &p, 10);
        while (*p == ' ') p++;
        /* (comm) — comm may contain spaces and parentheses, so find
         * the LAST ')' before reading further. */
        if (*p != '(') return -1;
        p++;
        const char *comm_start = p;
        const char *comm_end = strrchr(p, ')');
        if (!comm_end) return -1;
        size_t clen = (size_t)(comm_end - comm_start);
        if (clen >= comm_max) clen = comm_max - 1;
        memcpy(comm_out, comm_start, clen);
        comm_out[clen] = '\0';
        p = (char *)comm_end + 1;
        while (*p == ' ') p++;
        /* state */
        *state = *p ? *p : '?';
        if (*p) p++;
        while (*p == ' ') p++;
        /* ppid */
        *ppid = (int)strtol(p, &p, 10);
        return 0;
    }

    int main(int argc, char **argv)
    {
        (void)argc; (void)argv;
        DIR *dir = opendir("/proc");
        if (!dir) {
            fprintf(stderr, "ps: opendir(/proc): %s\n", strerror(errno));
            return 1;
        }
        printf("  PID  PPID S COMMAND\n");

        struct dirent *de;
        int rows = 0;
        while ((de = readdir(dir)) != NULL) {
            if (!is_all_digits(de->d_name)) continue;
            char path[64];
            snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
            int pid = 0, ppid = 0;
            char state = '?';
            char comm[64] = {0};
            if (parse_stat(path, &pid, &ppid, &state,
                           comm, sizeof(comm)) != 0)
                continue;
            printf("%5d %5d %c %s\n", pid, ppid, state, comm);
            rows++;
        }
        closedir(dir);
        if (rows == 0)
            fprintf(stderr, "ps: no processes found in /proc — is yos's "
                            "procfs mounted?\n");
        return 0;
    }
    YOSPS_EOF

    # ── minimal text-only libxo shim ──────────────────────────────────
    # df, wc (and other FreeBSD utilities) drive output through
    # libxo, which can emit text / XML / JSON / HTML. We only need
    # text mode in the wasm sandbox. Real libxo is 8.5kloc + locale +
    # encoder plugins; instead we ship a 200-line shim that parses
    # the `{role:name/text-fmt/enc-fmt}` token grammar enough to feed
    # the text-fmt + remaining varargs to vprintf. Tools link this
    # alongside their own object files; the symbols never resolve as
    # imports.
    mkdir -p "$STAGE/libxo/libxo"
    cat > "$STAGE/libxo/libxo/xo.h" <<'XOH_EOF'
    #ifndef _YOS_LIBXO_H_
    #define _YOS_LIBXO_H_
    #include <stdarg.h>
    #include <stdio.h>
    typedef struct yos_xo_handle xo_handle_t;
    /* Style/flag enums consumers reference; values irrelevant here. */
    enum { XO_STYLE_TEXT = 0, XO_STYLE_XML, XO_STYLE_JSON, XO_STYLE_HTML };
    /* Flags. */
    #define XOF_WARN          0x00000001
    #define XOF_FLUSH         0x00000002
    #define XOF_PRETTY        0x00000004
    #define XOF_DTRT          0x00000008
    int          xo_parse_args(int argc, char **argv);
    xo_handle_t *xo_create_to_file(FILE *fp, unsigned style, unsigned flags);
    int          xo_finish(void);
    int          xo_emit(const char *fmt, ...);
    int          xo_emit_h(xo_handle_t *xop, const char *fmt, ...);
    void         xo_attr(const char *name, const char *fmt, ...);
    void         xo_open_container(const char *name);
    void         xo_close_container(const char *name);
    void         xo_open_list(const char *name);
    void         xo_close_list(const char *name);
    void         xo_open_instance(const char *name);
    void         xo_close_instance(const char *name);
    void         xo_warn(const char *fmt, ...);
    void         xo_warnx(const char *fmt, ...);
    void         xo_err(int eval, const char *fmt, ...);
    void         xo_errx(int eval, const char *fmt, ...);
    void         xo_error(const char *fmt, ...);
    #endif
    XOH_EOF
    cat > "$STAGE/libxo/yos_libxo_shim.c" <<'XOC_EOF'
    /* yos minimal libxo shim — text-only.
     *
     * Translates xo_emit("{role:name/text-fmt/enc-fmt}…", ...)
     * into a single vprintf() of the concatenated text-fmts plus
     * any literal characters between tokens. xo_attr / xo_open_*
     * etc. are no-ops in text mode (they only matter for XML / JSON
     * / HTML emit paths we don't ship). */
    #include <err.h>
    #include <errno.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <libxo/xo.h>

    int xo_parse_args(int argc, char **argv) { (void)argv; return argc; }
    xo_handle_t *xo_create_to_file(FILE *fp, unsigned s, unsigned f)
    { (void)fp; (void)s; (void)f; return (xo_handle_t *)1; }
    int xo_finish(void) { fflush(stdout); return 0; }
    void xo_attr(const char *n, const char *f, ...)
    { (void)n; (void)f; }
    void xo_open_container(const char *n)  { (void)n; }
    void xo_close_container(const char *n) { (void)n; }
    void xo_open_list(const char *n)       { (void)n; }
    void xo_close_list(const char *n)      { (void)n; }
    void xo_open_instance(const char *n)   { (void)n; }
    void xo_close_instance(const char *n)  { (void)n; }

    static void
    yos_xo_translate(const char *fmt, char *out, size_t outsz,
                     const char *defaults_for_T)
    {
        (void)defaults_for_T;
        size_t op = 0;
        while (*fmt && op + 1 < outsz) {
            if (*fmt != '{') {
                out[op++] = *fmt++;
                continue;
            }
            fmt++;
            /* role chars: zero or more letters before ':' */
            while (*fmt && *fmt != ':' && *fmt != '/' && *fmt != '}') fmt++;
            if (*fmt == ':') fmt++;
            /* name: chars up to '/' or '}' — emitted verbatim if no
             * format spec follows, otherwise discarded. */
            const char *name = fmt;
            while (*fmt && *fmt != '/' && *fmt != '}') fmt++;
            size_t nlen = (size_t)(fmt - name);
            if (*fmt == '/') {
                /* text-fmt — copy verbatim into out. */
                fmt++;
                while (*fmt && *fmt != '/' && *fmt != '}' && op + 1 < outsz) {
                    out[op++] = *fmt++;
                }
                /* enc-fmt — discard. */
                if (*fmt == '/') {
                    fmt++;
                    while (*fmt && *fmt != '}') fmt++;
                }
            } else if (nlen > 0) {
                size_t n = nlen < (outsz - op - 1) ? nlen : (outsz - op - 1);
                memcpy(out + op, name, n);
                op += n;
            }
            if (*fmt == '}') fmt++;
        }
        out[op] = 0;
    }

    int xo_emit(const char *fmt, ...)
    {
        char buf[8192];
        va_list ap;
        yos_xo_translate(fmt, buf, sizeof(buf), NULL);
        va_start(ap, fmt);
        int rc = vprintf(buf, ap);
        va_end(ap);
        return rc;
    }

    int xo_emit_h(xo_handle_t *xop, const char *fmt, ...)
    {
        (void)xop;
        char buf[8192];
        va_list ap;
        yos_xo_translate(fmt, buf, sizeof(buf), NULL);
        va_start(ap, fmt);
        int rc = vprintf(buf, ap);
        va_end(ap);
        return rc;
    }

    /* err/warn-style: forward to libyos_stubs's err/warn (linked
     * into every tool via -lyos_stubs). */
    void xo_warn(const char *fmt, ...)
    {
        int saved = errno;
        va_list ap; va_start(ap, fmt);
        vwarn(fmt, ap);
        va_end(ap);
        errno = saved;
    }
    void xo_warnx(const char *fmt, ...)
    { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
    void xo_err(int eval, const char *fmt, ...)
    { va_list ap; va_start(ap, fmt); verr(eval, fmt, ap); va_end(ap); }
    void xo_errx(int eval, const char *fmt, ...)
    { va_list ap; va_start(ap, fmt); verrx(eval, fmt, ap); va_end(ap); }
    void xo_error(const char *fmt, ...)
    { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
    XOC_EOF

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
