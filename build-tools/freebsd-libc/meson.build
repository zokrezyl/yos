# build-tools/freebsd-libc — Tier 2 sidecar wasm.
#
# yos owns every libc function; for some we can't just call host
# libc (variadic printf family, FreeBSD-specific behaviour, fns
# missing on certain hosts). For those, yos compiles selected
# FreeBSD libc source files to wasm32 with `clang -target wasm32`
# and packages them into `libc-pure.wasm`. yos's host binary loads
# this sidecar at startup into a secondary wasm3 instance and
# routes the matching env imports through it.
#
# What goes in libc-pure.wasm:
#   - hand-written shim `tier2_demo.c` (plumbing test for the sidecar
#     pipeline; will be removed once a real FreeBSD piece is in)
#   - selected files from build-tools/freebsd/src/usr/src/lib/libc/
#     (added on demand as nvim/its deps surface need them)
#
# Output: <build>/build-tools/freebsd-libc/libc-pure.wasm
# yos host runtime locates it via a meson-injected absolute path.

# Use repo's `wasm-clang` shim for wasm32 cross-compilation. On Linux
# it's a thin passthrough; on darwin it strips nix's injected `-arch`/
# `-mmacos-version-min=...` so wasm-ld doesn't choke. See the script
# header for the gory details.
wasm_cc = find_program(meson.project_source_root() / 'build-tools' / 'wasm-clang',
                       required: true)

# Per-file wasm32 compile flags. -nostdlib because libc-pure.wasm
# IS our libc — anything it can't resolve locally must be an env
# import resolved by yos. -fno-builtin so clang doesn't lower
# `printf("foo")` to puts and break our routing. -O2 for sane
# perf.
freebsd_libc_wasm_cflags = [
  '-target', 'wasm32-unknown-unknown',
  '-nostdlib',
  '-ffreestanding',
  '-fno-builtin',
  '-O2',
  '-Wall',
  '-Wno-unused-parameter',
  # FreeBSD i386 headers — already curated by build-tools/freebsd/.
  # Lets us compile FreeBSD libc .c files verbatim (#include <string.h>,
  # <sys/types.h>, …).
  '-isystem',
  meson.project_build_root() / 'build-tools' / 'freebsd' / 'headers-i386' / 'usr' / 'include',
  # Allow taking address of fns (qsort comparator etc.) — needs the
  # function table exported.
  '-Wl,--no-entry',
  '-Wl,--export-all',
  '-Wl,--allow-undefined',
]

# Tier-2 wasm sources. Pulls in FreeBSD libc .c files verbatim
# (no patches, no copies — we point the compiler at them in the
# fetched FreeBSD source tree). Locally-sourced files (the demo,
# any yos shims) live under sources/.
freebsd_src_root = meson.project_build_root() / 'build-tools' / 'freebsd' / 'src' / 'usr' / 'src' / 'lib' / 'libc'

libc_pure_sources = [
  files('sources/tier2_demo.c'),
  freebsd_src_root / 'string'  / 'strlcpy.c',
  freebsd_src_root / 'stdlib'  / 'abs.c',
]

libc_pure_wasm = custom_target(
  'libc-pure.wasm',
  input            : libc_pure_sources,
  output           : 'libc-pure.wasm',
  command          : [wasm_cc, freebsd_libc_wasm_cflags,
                      '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default : true,
)

# Absolute path string (resolved at meson-time) so yos's main.c can
# reach the file at runtime without scanning. Plumbed through a
# generated config header.
libc_pure_path = libc_pure_wasm.full_path()
