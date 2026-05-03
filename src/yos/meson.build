# yos host runtime.
#
#   types.h, ydebug.h    runtime struct + debug macros (top-level)
#   main.c               entry: load wasm, link imports, run _start
#
#   impl/proc.c          yos_fork / yos_getpid / yos_kill / yos_waitpid / ...
#   impl/pthread.c       yos_pthread_create / _mutex_* / _cond_* / TLS pool
#   impl/vfs.c           yos_open / yos_read / yos_write / yos_close / ...
#   impl/mem.c           yos_brk / yos_mmap / yos_mprotect / ...
#   impl/sig.c           yos_sigaction / yos_sigprocmask
#   impl/f128.c          __float128 soft-float helpers
#
#   vfs/                 mount table, file glue, procfs synth
#   generated/           struct_convert.{c,h} (wasm32<->host64 shape converter,
#                        from yos-private's codegen; will be locally regenerated
#                        by src/yos/codegen later).
#
#   codegen/             the codegen pipeline itself: extract.py, compare.py,
#                        analyse.py, bridge.py, hooks.yaml. Outputs go to
#                        <build>/src/yos/codegen/.
#
# Function-naming rule (per CLAUDE.md): every libc-shaped subsystem entry is
# `yos_<libc-name>` — no `_proc_` / `_vfs_` / `_mem_` / `_sig_` infix. Internal
# helpers (yos_proc_alloc, yos_vfs_resolve_path, etc.) keep their infix because
# they're not libc functions.
#
# Include layout (matches yos-private's `-Isrc`):
#   -Isrc                so `"yos/types.h"` resolves to src/yos/types.h
#   -Isrc/yos            so `"impl/proc.h"`, `"vfs/file.h"` etc. resolve
#   -Isrc/yos/generated  so `"host64_structs.h"` etc. resolve
#   -Iinclude            so `<yos/yos_pthread.h>` resolves

threads_dep = dependency('threads')
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)

subdir('codegen')        # api_bridge_dep, yos_imports.h

yos_inc = include_directories(
  '..',                  # `yos/types.h`
  '../../include',       # `yos/yos_pthread.h`
  '.',                   # `impl/<area>.h`, `vfs/<x>.h`, `generated/<x>.h`
  'generated',           # `host64_structs.h`, `struct_convert.h`
  'vfs',                 # vfs/*.c sees sibling `file.h` etc.
)

yos_runtime_sources = files(
  'main.c',
  'impl/proc.c',
  'impl/pthread.c',
  'impl/vfs.c',
  'impl/mem.c',
  'impl/sig.c',
  'impl/f128.c',
  'impl/aliases.c',
  'impl/alloc.c',
  'impl/callback.c',
  'impl/signal.c',
  'impl/strto.c',
  'impl/sysctl.c',
  'impl/tier2.c',
  'impl/printf.c',
  'vfs/file.c',
  'vfs/mount.c',
  'vfs/procfs.c',
  # struct_convert: wasm32<->host64 layout-conversion helpers; pure
  # data transformation, no kernel calls. See CLAUDE.md non-negotiable
  # #5 for why generated/syscall_dispatch.c + syscall_handlers.c are
  # intentionally NOT linked.
  'generated/struct_convert.c',
)

yos_exe = executable(
  'yos',
  yos_runtime_sources,
  include_directories : yos_inc,
  dependencies : [
    wasm3_dep,
    api_bridge_dep,      # m3w_<fn> wrappers + yos_brg_link_imports()
    mimalloc_dep,        # impl/alloc.c — guest malloc backed by mimalloc
    threads_dep,
    m_dep,
  ],
  c_args : [
    '-D_GNU_SOURCE',
    # Path to the Tier-2 sidecar wasm. yos opens this at startup.
    '-DYOS_LIBC_PURE_PATH="' + libc_pure_path + '"',
    '-Wno-unused-parameter',
    '-Wno-unused-but-set-variable',
    '-Wno-unused-variable',
    '-Wno-unused-function',
    '-Wno-implicit-fallthrough',
    '-Wno-format-overflow',
  ],
  install : false,
)
