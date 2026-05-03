# wasm3 — vendored locally in source/.
#
# Source is yos-private's wasm3 fork (zokrezyl/wasm3 with the
# pthread-as-syscall + sibling-runtime extensions). Treated like
# locally-maintained code, not fetched. Copied wholesale once; future
# updates by re-running tools/sync-wasm3.sh (TBD).

wasm3_sources = files(
  'source/m3_api_libc.c',
  'source/m3_atomic.c',
  'source/m3_bind.c',
  'source/m3_code.c',
  'source/m3_compile.c',
  'source/m3_core.c',
  'source/m3_emit.c',
  'source/m3_env.c',
  'source/m3_exec.c',
  'source/m3_function.c',
  'source/m3_info.c',
  'source/m3_module.c',
  'source/m3_optimize.c',
  'source/m3_parse.c',
)

wasm3_lib = static_library(
  'wasm3',
  wasm3_sources,
  c_args : [
    '-D_GNU_SOURCE',
    # Enable wasm backtraces — main.c's abort/assert handlers use them
    # to tell us WHICH wasm function bailed out.
    '-Dd_m3RecordBacktraces=1',
    # m3_core.c sets M3_IMPLEMENT_ERROR_STRINGS itself so error globals
    # land there. We don't need it project-wide.
    '-Wno-unused-parameter',
    '-Wno-unused-but-set-variable',
    '-Wno-unused-variable',
    '-Wno-implicit-fallthrough',
    '-Wno-unused-function',
    '-Wno-array-bounds',
    '-Wno-stringop-overread',
    '-Wno-stringop-overflow',
    '-Wno-maybe-uninitialized',
    '-Wno-format-overflow',
    '-Wno-tautological-compare',
    '-Wno-pedantic',
    '-fno-strict-aliasing',  # wasm3 reads/writes wasm cells with type-punning
  ],
  include_directories : include_directories('source'),
  install : false,
)

wasm3_dep = declare_dependency(
  link_with           : wasm3_lib,
  include_directories : include_directories('source'),
)
