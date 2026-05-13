# tests/ut/libc — wasm32 guest tests for the libc-by-import surface.
#
# Each test_*.c compiles to wasm32 with clang, then runs through yos.
# The runner script reads the test's `Expected: exit N, stdout contains
# "..."` header to know what counts as a pass.

wasm_cc  = find_program(meson.project_source_root() / 'build-tools' / 'wasm-clang',
                        required: true)
wasm_opt = find_program('wasm-opt', required: true)  # Binaryen — for asyncify

# Tests run after `yos` is built; meson.test() handles the dep.
yos_bin = yos_exe.full_path()

# Per-test wasm32 compile flags. -nostdlib because the test brings its
# own minimal types and only uses imports — no libc baggage. -Wl flags
# tell wasm-ld to skip _start auto-detection and export the symbol
# yos calls into.
sysroot_dir = meson.project_build_root() / 'sysroot'

wasm_cflags = [
  '-target', 'wasm32-unknown-unknown',
  '-nostdlib',
  '--sysroot=' + sysroot_dir,
  '-isystem', sysroot_dir / 'usr' / 'include',
  '-Wl,--no-entry',
  '-Wl,--export=_start',
  '-Wl,--export-all',         # function table exposed (pthread_create
                              # reads it to look up start_routine)
  '-Wl,--allow-undefined',    # unresolved fn refs become env.<name>
                              # imports automatically — that's exactly
                              # the yos contract.
  '-O2',
]

libc_tests = [
  'test_redecl_merge',
  'test_write_passthrough',
  'test_exit_code',
  'test_generated_imports',
  'test_fork_basic',
  'test_pthread_create_join',
  'test_tier2_sidecar',
  'test_tier2_abs',
  'test_freebsd_headers',
  'test_malloc_basic',
  # nvim-on-darwin trouble-area pins (tmp/nvim-runtime-issues.md).
  # Each isolates one fd-bridge corner that the embedded nvim server
  # depends on. Pass on Linux; designed to surface darwin regressions.
  'test_socketpair_flags',
  'test_fcntl_dupfd_cloexec',
  'test_dup2_pipe_pair',
  'test_socketpair_sotype_isatty',
  'test_kqueue_evfilt_read',
  'test_ioctl_fionbio',
  'test_open_oflags',
  # End-to-end reproducer for the nvim embedded-server failure on
  # darwin. Walks channel_from_stdio + uv_pipe_open + uv_read_start
  # in nvim's order; each step has a unique exit code so a failing
  # run pinpoints the broken bridge layer.
  'test_nvim_channel_from_stdio',
  # Same reproducer but across a fork — the actual nvim embedded
  # server is a forked child, and the fd_map handoff via
  # yos_fd_fork_dup is part of the suspect surface. Needs asyncify.
  'test_fork_channel_from_stdio',
  # uv_guess_handle algorithm reproducer. Probes isatty, fstat,
  # getsockname, getsockopt(SO_TYPE) on a socketpair end in libuv's
  # exact order. The getsockname check pins the BSD-host bug in
  # src/yos/impl/posix.c:96 (host_buf[0]|host_buf[1]<<8 reads sa_len
  # | sa_family<<8 on darwin/FreeBSD instead of the Linux uint16
  # sa_family at offset 0).
  'test_uv_guess_handle',
  # Probes whether the bridge writes st_mode to the same offset that
  # the FreeBSD sysroot's struct stat reads it from. A mismatch would
  # explain why uv_guess_handle on a SOCK fd silently returns the
  # wrong type without ever calling getsockname.
  'test_stat_layout_probe',
]

# Path to the generator's output dir, exposed by src/yos/codegen.
# api_bridge_outputs[2] is yos_imports.h — used to derive the include path.
yos_imports_dir = meson.project_build_root() / 'src' / 'yos' / 'codegen'

# Combined performance + stress test. Different from the others: links
# against crt1.o so main() gets argv (needed because the test execve's
# itself, passing argv[0] as the wasm path). Built standalone, not
# inside the libc_tests foreach.
perf_stress_raw = custom_target(
  'test_perf_stress-raw.wasm',
  input            : files('test_perf_stress.c'),
  output           : 'test_perf_stress-raw.wasm',
  depends          : sysroot_skel,
  command          : [wasm_cc,
                      '-target', 'wasm32-unknown-unknown',
                      '-nostdlib',
                      '--sysroot=' + sysroot_dir,
                      '-isystem', sysroot_dir / 'usr' / 'include',
                      '-L', sysroot_dir / 'usr' / 'lib',
                      '-Wl,--no-entry',
                      '-Wl,--allow-undefined',
                      '-Wl,--export=_start',
                      '-Wl,--export=main',
                      # pthread_create looks up start_routine by index
                      # into the wasm function table; without
                      # --export-all the table is empty and every
                      # thread traps with "fn_idx N out of table".
                      '-Wl,--export-all',
                      '-O2',
                      sysroot_dir / 'usr' / 'lib' / 'crt1.o',
                      '-o', '@OUTPUT@', '@INPUT0@'],
  build_by_default : true,
)
perf_stress_wasm = custom_target(
  'test_perf_stress.wasm',
  input            : perf_stress_raw,
  output           : 'test_perf_stress.wasm',
  command          : [wasm_opt, '--asyncify', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default : true,
)
test('test_perf_stress',
  py,
  args : [
    files('run_libc_test.py'),
    '--yos',     yos_bin,
    '--src',     files('test_perf_stress.c'),
    '--wasm',    perf_stress_wasm,
    '--timeout', '90',
  ],
  suite   : 'libc',
  # Stress test spawns many forks + threads + execve'd processes; the
  # default 30 s per-test timeout is tight when the host is under
  # load. 120 s is plenty even on a slow VM.
  timeout : 120,
)

foreach t : libc_tests
  src = files(t + '.c')

  # Tests that include the auto-generated yos_imports.h need the
  # codegen output dir on their include path AND a dep on the bridge
  # custom_target so meson regenerates the header before compiling.
  extra_inputs = []
  extra_cflags = []
  if t in ['test_generated_imports', 'test_fork_basic',
           'test_pthread_create_join', 'test_tier2_abs']
    extra_inputs += [api_bridge_outputs]
    extra_cflags += ['-I' + yos_imports_dir]
  endif

  raw_wasm = custom_target(
    t + '-raw.wasm',
    input            : [src] + extra_inputs,
    output           : t + '-raw.wasm',
    # Depend on the sysroot stamp so wasm tests rebuild when FreeBSD
    # headers change. sysroot_skel is a custom_target.
    depends          : sysroot_skel,
    command          : [wasm_cc, wasm_cflags, extra_cflags,
                        '-o', '@OUTPUT@', '@INPUT0@'],
    build_by_default : true,
  )

  # Tests that exercise fork / setjmp / longjmp need the Binaryen
  # asyncify pass applied so the host runtime can unwind/rewind the
  # wasm call stack. Plain stateless tests skip the pass.
  needs_asyncify = t in ['test_fork_basic', 'test_fork_channel_from_stdio']

  if needs_asyncify
    wasm = custom_target(
      t + '.wasm',
      input            : raw_wasm,
      output           : t + '.wasm',
      command          : [wasm_opt, '--asyncify', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default : true,
    )
  else
    wasm = raw_wasm
  endif

  test(t,
    py,
    args : [
      files('run_libc_test.py'),
      '--yos',  yos_bin,
      '--src',  src,
      '--wasm', wasm,
    ],
    suite : 'libc',
  )
endforeach
