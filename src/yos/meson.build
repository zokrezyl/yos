# yos host runtime.
#
#   types.h              runtime struct (top-level). Trace/debug macros
#                        (ydebug/yinfo/ywarn/yerror/ytrace) come from
#                        include/yos/ytrace/ytrace.h.
#   main.c               entry: load wasm, link imports, run _start
#
#   impl/proc/   process subsystem: fork+exec+wait+kill, pthread+TLS pool,
#                signals. Owns the proc table + asyncify fork mechanics.
#   impl/io/     POSIX I/O bridges: open/read/write/close + fd_map,
#                opendir/readdir DIR table, host FILE* table, FIFO
#                emulation (mkfifo-forbidding sandboxes), PTY emulation
#                (posix_openpt-forbidding sandboxes). NOT the same as
#                src/yos/vfs/ — that's the custom virtual filesystem
#                layer (mount table, procfs, file_operations vtable);
#                impl/io/io.c dispatches to it for /proc lookups.
#   impl/mem/    brk + mmap inside guest linear memory, malloc/free.
#   impl/libc/   pure / near-pure libc surface — printf, posix passthroughs,
#                env, regex, kqueue, sysctl, syslog, mbwc, etc.
#   impl/errno_helpers.h  shared by everything in impl/.
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
# libpython 3.12 (embedded) — links the real CPython interpreter into
# the yos host so wasm guests can drive it through env.Py_* bridges
# (impl/libpython.c). Optional: if libpython isn't installed, the
# bridges still compile but every Py_* call traps with unresolved-symbol.
with_libpython_opt = get_option('with_libpython')
if with_libpython_opt.disabled()
  python_embed_dep = dependency('', required: false)
  have_libpython = false
else
  python_embed_dep = dependency('python3-embed', required: with_libpython_opt)
  have_libpython = python_embed_dep.found()
endif
# iconv: glibc folds it into libc; darwin/BSD ship a separate libiconv
# whose symbols start with `iconv_*` (and `__iconv_*` for the GNU
# extras). yos's bridge calls those names, so we need explicit linkage
# wherever it isn't already in libc.
iconv_dep = cc.find_library('iconv', required: false)
# msgpack-c — host-side library, used by yctl/yctl.c for the
# msgpack-RPC introspection daemon (--yctl-socket PATH).
# Upstream pkg-config name flipped between '-c' and bare 'msgpack'
# across versions; accept either.
with_yctl_opt = get_option('with_yctl')
if with_yctl_opt.disabled()
  msgpack_dep = dependency('', required: false)
  have_yctl = false
else
  msgpack_dep = dependency('msgpack-c', required: false)
  if not msgpack_dep.found()
    msgpack_dep = dependency('msgpack', required: with_yctl_opt)
  endif
  have_yctl = msgpack_dep.found()
endif

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

# ── per-platform impl/ slice selection ────────────────────────────────
#
# Most libc bridges in impl/ are platform-neutral and built everywhere.
# Where a bridge has platform-specific behaviour (Linux-only syscalls,
# darwin Mach ports, sandbox restrictions on iOS/tvOS), the divergent
# code is sliced into `<area>-<platform>.c` files and the platform-
# neutral file `<area>.c` only carries the shared helpers (or is empty
# when nothing is shared). NO #ifdef inside any of those files — meson
# picks the right slice based on host.
#
# Slice categories (extend with -freebsd.c, -windows.c as needed):
#   <area>-linux.c       linux hosts
#   <area>-darwin.c      every libSystem host (macos + ios + tvos + ios-sim)
#   <area>-darwin-app.c  sandboxed Apple-app hosts (ios + tvos) — no
#                        Mach-exception ports, no posix_openpt /dev/ptmx,
#                        no host mkfifo(2), no audio-session pause when
#                        backgrounded. macOS is NOT in this set.
#   <area>-macos.c       macOS only — Mach ports available, full sandbox
#                        privileges of a user binary
#   <area>-ios.c         iOS device only
#   <area>-tvos.c        tvOS device only
#   <area>-ios-sim.c     iOS Simulator only (libSystem but no sandbox)
#
# Per-file rule (per user request): if a function name shows up across
# multiple slices and the *bodies* diverge, the function declaration
# lives in `<area>.c` as a helper that the slices call differently.
# Library entry points (`yos_<libc-name>`) implementations live in the
# slice when the body diverges; in `<area>.c` when it does not.
is_ios_app  = get_option('ios_unblock')
is_tvos_app = get_option('tvos_unblock')
is_darwin_app = is_ios_app or is_tvos_app
is_macos      = (host_os == 'darwin') and (not is_darwin_app)

impl_platform_sources = []
if host_os == 'linux'
  impl_platform_sources += files(
    'impl/proc/sig-linux.c',
    'impl/libc/kqueue-linux.c',
    'impl/libc/posix-linux.c',
    'impl/main-linux.c',
    'impl/io/io-linux.c',
  )
elif host_os == 'darwin'
  impl_platform_sources += files(
    'impl/proc/sig-darwin.c',
    'impl/libc/kqueue-darwin.c',
    'impl/libc/posix-darwin.c',
    'impl/io/io-darwin.c',
  )
  if is_darwin_app
    # iOS / tvOS app bundle: Mach exception ports + sigaltstack are
    # both unavailable. main-darwin-app.c provides no-op stubs.
    impl_platform_sources += files('impl/main-darwin-app.c')
  else
    # macOS: full host privileges — Mach exception handler + real
    # sigaltstack.
    impl_platform_sources += files('impl/main-macos.c')
  endif
endif

yos_runtime_sources = files(
  'main.c',
  # ── proc/ ── fork / exec / wait / kill, pthread + TLS pool, signals.
  'impl/proc/proc.c',
  'impl/proc/pthread.c',
  'impl/proc/sig.c',
  'impl/proc/signal.c',
  # ── vfs/ ── open/read/write/close + fd_map; opendir/readdir DIR
  # table; FILE* table; FIFO emulation (mkfifo-forbidding sandboxes);
  # PTY emulation (posix_openpt-forbidding sandboxes).
  'impl/io/io.c',
  'impl/io/dir.c',
  'impl/io/file.c',
  'impl/io/fifo.c',
  'impl/io/pty.c',
  'impl/io/cv_stat.c',
  # ── mem/ ── brk + mmap inside guest linear memory, malloc/free.
  'impl/mem/mem.c',
  'impl/mem/alloc.c',
  # ── libc/ ── pure / near-pure libc surface (no per-ctx state
  # beyond what the auto-bridge can derive). Posix passthroughs,
  # printf family, multibyte/widechar, regex, kqueue, sysctl,
  # syslog, env, getopt, timezone, password DB, etc.
  'impl/libc/posix.c',
  'impl/libc/printf.c',
  'impl/libc/scanf.c',
  'impl/libc/env.c',
  'impl/libc/getopt.c',
  'impl/libc/tz.c',
  'impl/libc/libc_globals_link.c',
  'impl/libc/aliases.c',
  'impl/libc/callback.c',
  'impl/libc/cv_trace.c',
  'impl/libc/f128.c',
  'impl/libc/freebsd_userland.c',
  'impl/libc/fmtcheck_freebsd.c',
  'impl/libc/kqueue.c',
  'impl/libc/mbwc.c',
  'impl/libc/pwd.c',
  'impl/libc/random.c',
  'impl/libc/regex.c',
  'impl/libc/strto.c',
  'impl/libc/sysctl.c',
  'impl/libc/syslog_extras.c',
  # ytrace + yplatform — switchable trace-point machinery imported
  # verbatim from yetty (with yetty_yplatform → yos_yplatform).
  # ytrace.h defines ydebug/yinfo/ywarn/yerror/ytrace and the
  # ytime_start/ytime_report timer macros; ytrace.c provides the
  # registry, output formatting and runtime enable/disable.
  'ytrace/ytrace.c',
  # yperf — per-call profile recorder. See include/yos/yperf/yperf.h.
  # Hot path lives in vendored wasm3's op_Entry; this file owns
  # state, dumping and the symbol-walker callback wiring.
  'yperf/yperf.c',
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

# Optional sources, gated by feature switches. Each is paired with a
# preprocessor flag so main.c can compile out the wiring (#ifdef
# YOS_HAVE_YCTL / YOS_HAVE_LIBPYTHON).
optional_sources = []
optional_args    = []
optional_deps    = []
if have_yctl
  optional_sources += files('yctl/yctl.c')
  optional_args    += '-DYOS_HAVE_YCTL=1'
  optional_deps    += msgpack_dep
endif
if have_libpython
  optional_sources += files('impl/libc/libpython.c')
  optional_args    += '-DYOS_HAVE_LIBPYTHON=1'
  optional_deps    += python_embed_dep
endif

yos_common_c_args = [
  '-D_GNU_SOURCE',
  '-Wno-unused-parameter',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-variable',
  '-Wno-unused-function',
  '-Wno-implicit-fallthrough',
  '-Wno-format-overflow',
] + optional_args

yos_common_deps = [
  wasm3_dep,
  api_bridge_dep,      # m3w_<fn> wrappers + yos_brg_link_imports()
  threads_dep,
  m_dep,
  iconv_dep,           # darwin/BSD: separate libiconv
] + optional_deps

# tvOS / iOS app bundles need a static library, not an executable:
# the bundle's launcher (build-tools/tvos/launcher.m) provides main()
# + UIApplicationMain and pthread_create's a worker that jumps to
# yos_main(). Same sources as the executable, just compiled with
# -DYOS_AS_LIBRARY so main.c emits `yos_main` instead of `main`. See
# -Dbuild_static_lib=true.
#
# For the lib case we DON'T also build the executable, because that
# would give meson two targets named `yos` (ambiguous on the command
# line) and tvOS clang refuses to link an executable anyway without
# `main` (which the lib renames away).
if get_option('build_static_lib')
  yos_static = static_library(
    'yos',
    yos_runtime_sources,
    platform_sources,
    impl_platform_sources,
    optional_sources,
    include_directories : yos_inc,
    dependencies : yos_common_deps,
    c_args : yos_common_c_args + ['-DYOS_AS_LIBRARY=1'],
    install : false,
  )
else
  yos_exe = executable(
    'yos',
    yos_runtime_sources,
    platform_sources,
    impl_platform_sources,
    optional_sources,
    include_directories : yos_inc,
    dependencies : yos_common_deps,
    c_args : yos_common_c_args,
    install : false,
  )
endif
