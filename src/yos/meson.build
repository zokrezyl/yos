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
# iconv: glibc folds it into libc; darwin/BSD ship a separate libiconv
# whose symbols start with `iconv_*` (and `__iconv_*` for the GNU
# extras). yos's bridge calls those names, so we need explicit linkage
# wherever it isn't already in libc.
iconv_dep = cc.find_library('iconv', required: false)

subdir('codegen')        # api_bridge_dep, yos_imports.h

yos_inc = include_directories(
  '..',                  # `yos/types.h`
  '../../include',       # `yos/yos_pthread.h`, `yos/ytrace/ytrace.h`,
                         # `yos/yplatform/{time,thread,term}.h`
  '.',                   # `impl/<area>.h`, `vfs/<x>.h`, `generated/<x>.h`
  'generated',           # `host64_structs.h`, `struct_convert.h`
  'vfs',                 # vfs/*.c sees sibling `file.h` etc.
  'platform/posix',      # `platform.h` (Linux+darwin shared abstraction)
)

# Platform-specific host sources. Anything that diverges between Linux
# and darwin (gettid, …) lands in platform/posix/<x>-{linux,darwin}.c;
# Windows port will grow platform/windows/ later.
host_os = host_machine.system()
if host_os == 'linux'
  platform_sources = files('platform/posix/platform-linux.c')
elif host_os == 'darwin'
  platform_sources = files('platform/posix/platform-darwin.c')
else
  error('yos: unsupported host OS: ' + host_os)
endif

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
  'impl/cv_trace.c',
  'impl/env.c',
  'impl/file.c',
  'impl/kqueue.c',
  'impl/posix.c',
  'impl/fmtcheck_freebsd.c',
  'impl/random.c',
  'impl/signal.c',
  'impl/strto.c',
  'impl/freebsd_userland.c',
  'impl/sysctl.c',
  'impl/printf.c',
  'impl/dir.c',
  # ytrace + yplatform — switchable trace-point machinery imported
  # verbatim from yetty (with yetty_yplatform → yos_yplatform).
  # ytrace.h defines ydebug/yinfo/ywarn/yerror/ytrace and the
  # ytime_start/ytime_report timer macros; ytrace.c provides the
  # registry, output formatting and runtime enable/disable.
  'ytrace/ytrace.c',
  'yplatform/time/default.c',
  'yplatform/thread/default.c',
  'yplatform/term/default.c',
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
  platform_sources,
  include_directories : yos_inc,
  dependencies : [
    wasm3_dep,
    api_bridge_dep,      # m3w_<fn> wrappers + yos_brg_link_imports()
    threads_dep,
    m_dep,
    iconv_dep,           # darwin/BSD: separate libiconv
  ],
  c_args : [
    '-D_GNU_SOURCE',
    '-Wno-unused-parameter',
    '-Wno-unused-but-set-variable',
    '-Wno-unused-variable',
    '-Wno-unused-function',
    '-Wno-implicit-fallthrough',
    '-Wno-format-overflow',
  ],
  install : false,
)
