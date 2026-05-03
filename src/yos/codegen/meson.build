# src/yos/codegen — the whole codegen pipeline.
#
# Single subdir holds all codegen scripts + their inputs (hooks.yaml,
# entry_map.yaml). Outputs go to <build>/src/yos/codegen/.
#
# Pipeline (each stage's output is the next stage's input):
#
#   freebsd-headers-i386 ─┐
#                         ├─→ extract.py ──→ guest-api-i386-freebsd.yaml
#                         │                  + host-api.yaml
#   host-libc-snapshot   ─┘
#                                 │
#                                 ↓
#                            compare.py ──→ compare-report.yaml
#                                 │
#                                 ↓
#                            analyse.py ──→ analyse-report.yaml
#                                 │
#                                 ↓
#                            bridge.py  ──→ yos_bridge.{c,h}
#                            generate.py    yos_imports.h
#                                            yos_remap.{c,h}
#                                            hooks_todo.yaml
#                                            stats.txt
#
# `hooks.yaml` (in this dir, source-tree) drives bridge.py's per-fn
# routing: passthrough / custom_vfs / custom_proc / custom_pthread /
# custom_mem / custom_alloc / custom_sig / struct_convert / variadic /
# stub / runtime_owned. See CLAUDE.md "Codegen strategy" + the file
# header in hooks.yaml itself.

# ─── extract: guest + host API yaml ──────────────────────────────────

guest_api_yaml = custom_target(
  'guest-api-i386-freebsd',
  input            : freebsd_headers_i386,
  output           : 'guest-api-i386-freebsd.yaml',
  command          : [
    py, files('extract.py'),
    '--name',         'guest-i386-freebsd',
    '--no-stdinc',
    '--include-dir',  '@INPUT@/usr/include',
    '--root-marker',  'usr/include/',
    '--target',       'wasm32-unknown-unknown',
    '--enum',
    '--restrict-to',  'sys/',
    '--restrict-to',  'machine/',
    '--restrict-to',  'x86/',
    '--restrict-to',  'arpa/',
    '--restrict-to',  'net/',
    '--restrict-to',  'netinet/',
    '--restrict-to',  'netinet6/',
    '--restrict-to',  'vm/',
    '--restrict-to',  'stdio.h', '--restrict-to', 'stdlib.h',
    '--restrict-to',  'string.h', '--restrict-to', 'unistd.h',
    '--restrict-to',  'fcntl.h', '--restrict-to', 'errno.h',
    '--restrict-to',  'time.h',  '--restrict-to', 'signal.h',
    '--restrict-to',  'pthread.h',
    '--out',          '@OUTPUT@',
  ],
  build_by_default : false,
  console          : true,
)

host_api_yaml = custom_target(
  'host-api',
  input            : host_libc_snapshot,
  output           : 'host-api.yaml',
  command          : [
    py, files('extract.py'),
    '--name',         'host',
    '--include-dir',  '@INPUT@/include/01-include',
    '--include-dir',  '@INPUT@/include/02-include',
    '--include-dir',  '@INPUT@/include/03-x86_64-linux-gnu',
    '--include-dir',  '@INPUT@/include/04-include',
    '--root-marker',  '/usr/include/',
    '--root-marker',  '/usr/local/include/',
    '--root-marker',  'include/',
    '--enum',
    '--restrict-to',  'sys/',
    '--restrict-to',  'arpa/',
    '--restrict-to',  'net/',
    '--restrict-to',  'netinet/',
    '--restrict-to',  'stdio.h', '--restrict-to', 'stdlib.h',
    '--restrict-to',  'string.h', '--restrict-to', 'unistd.h',
    '--restrict-to',  'fcntl.h', '--restrict-to', 'errno.h',
    '--restrict-to',  'time.h',  '--restrict-to', 'signal.h',
    '--restrict-to',  'pthread.h',
    '--out',          '@OUTPUT@',
  ],
  build_by_default : false,
  console          : true,
)

alias_target('api', guest_api_yaml, host_api_yaml)

# ─── compare + analyse ──────────────────────────────────────────────

api_compare_report = custom_target(
  'api-compare-report',
  input            : [guest_api_yaml, host_api_yaml],
  output           : 'compare-report.yaml',
  command          : [
    py, files('compare.py'),
    '--guest', '@INPUT0@',
    '--host',  '@INPUT1@',
    '--out',   '@OUTPUT@',
  ],
  build_by_default : false,
  console          : true,
)

api_analyse_report = custom_target(
  'api-analyse-report',
  input            : [api_compare_report, guest_api_yaml, host_api_yaml],
  output           : 'analyse-report.yaml',
  command          : [
    py, files('analyse.py'),
    '--report',    '@INPUT0@',
    '--guest-api', '@INPUT1@',
    '--host-api',  '@INPUT2@',
    '--out',       '@OUTPUT@',
  ],
  build_by_default : false,
  console          : true,
)

alias_target('api-compare', api_compare_report)
alias_target('api-analyse', api_analyse_report)

# ─── generate: remap + hooks_todo + stats ───────────────────────────

api_generate_outputs = custom_target(
  'api-generate-outputs',
  input            : api_analyse_report,
  output           : ['yos_remap.h', 'yos_remap.c', 'hooks_todo.yaml', 'stats.txt'],
  command          : [
    py, files('generate.py'),
    '--analyse', '@INPUT@',
    '--out-dir', '@OUTDIR@',
  ],
  build_by_default : false,
  console          : true,
)

# ─── bridge: yos_bridge.{c,h} + yos_imports.h ───────────────────────

api_bridge_outputs = custom_target(
  'api-bridge-outputs',
  input            : [api_analyse_report, guest_api_yaml, host_api_yaml],
  output           : ['yos_bridge.h', 'yos_bridge.c', 'yos_imports.h'],
  command          : [
    py, files('bridge.py'),
    '--analyse',   '@INPUT0@',
    '--guest-api', '@INPUT1@',
    '--host-api',  '@INPUT2@',
    '--hooks',     files('hooks.yaml'),
    '--out-dir',   '@OUTDIR@',
  ],
  build_by_default : false,
  console          : true,
)

alias_target('api-generate', api_generate_outputs, api_bridge_outputs)
alias_target('api-bridge',   api_bridge_outputs)

# ─── compile generated bridges into a static lib ────────────────────

# We need `yos/types.h` (struct yos_exec_ctx) on the include path —
# yos-private convention `-Isrc` resolves "yos/types.h" to src/yos/.
# From this dir (src/yos/codegen) we reach up two levels to `src/`.
yos_types_inc = include_directories('../../..' / 'src')
yos_inc       = include_directories('../../..' / 'include')

# Bridge .c also pulls in `"impl/tier2.h"` for from_freebsd_src
# routings; that resolves via -Isrc/yos.
yos_impl_inc  = include_directories('..')

api_bridge_lib = static_library(
  'yos_api_bridge',
  api_bridge_outputs[1],   # yos_bridge.c
  api_generate_outputs[1], # yos_remap.c
  include_directories : [yos_types_inc, yos_inc, yos_impl_inc],
  dependencies        : [wasm3_dep],
  c_args : [
    '-D_GNU_SOURCE',
    '-Wno-deprecated-declarations',
    '-Wno-implicit-function-declaration',
    '-Wno-incompatible-pointer-types',
    '-Wno-discarded-qualifiers',
    '-Wno-pointer-sign',
    '-Wno-int-conversion',
    '-Wno-unused-parameter',
    '-Wno-unused-variable',
  ],
  install : false,
)

# link_whole so every bridge symbol lands in the binary even when
# yos_brg_link_imports() is the only direct caller (the linker
# otherwise drops the m3w_<name> wrappers as unreferenced).
api_bridge_dep = declare_dependency(
  link_whole          : api_bridge_lib,
  include_directories : [
    include_directories('.'),
    yos_types_inc,
    yos_inc,
  ],
)
