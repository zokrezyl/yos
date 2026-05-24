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
  'test_freebsd_headers',
  'test_malloc_basic',
  # nvim-on-darwin trouble-area pins (tmp/nvim-runtime-issues.md).
  # Each isolates one fd-bridge corner that the embedded nvim server
  # depends on. Pass on Linux; designed to surface darwin regressions.
  'test_socketpair_flags',
  'test_fcntl_dupfd_cloexec',
  'test_dup2_pipe_pair',
  'test_closefrom_preserves_stdio',
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
  # Asyncify-leak regression. The forked child's first stderr write
  # must be byte-clean — no 0xff prefix from the rewound stack frame.
  # The visible symptom of this leak in openssh was "Bad file
  # descriptor" rendered as 0xff×8 + "ad file descriptor". The yos
  # workaround in vfs.c::yos_write only catches pure-0xff payloads;
  # this test exercises a MIXED payload (single byte 'X') over a pipe
  # so the workaround is bypassed and the raw leak shows.
  'test_post_fork_stderr_clean',
  # Stdio (fprintf) write from a forked child must reach the host
  # stderr. zsh-wasm's "command not found" is emitted from a forked
  # child via fputc on stderr — under yos that output disappears
  # because the child exits without an stdio flush.
  'test_fork_fprintf_stderr',
  # FreeBSD libc strmode(3): mode_t → "drwxr-xr-x " column ls -l
  # prints. Pure / stateless; hand-bridged in impl/posix.c because
  # glibc has no equivalent and the codegen ENOSYS stub left every
  # ls -l line with a blank mode column.
  'test_strmode',
  # yos_lpathconf bridge errno contract — pins the fix for the fts
  # info-leak ls printed before yos_lpathconf called yos_errno_neg
  # ("./<file>: Undefined error: 0"). See impl/libc/posix.c::yos_lpathconf.
  'test_lpathconf_errno',
  # fopen → fileno → fstat round-trip. Pre-fix yos_fileno returned
  # the host fd, which couldn't be looked up in ctx->fd_map; every
  # subsequent fstat/read/fcntl EBADF'd. ssh's "fstat /Users/.../
  # .ssh/config: Bad file descriptor" pinned by this test.
  'test_fopen_fileno_fstat',
  # lstat on a symlink: AT_SYMLINK_NOFOLLOW must be translated to
  # the host's bit (FreeBSD 0x200 → darwin 0x20) so the call
  # doesn't collapse to stat(). And the result's mode bits must be
  # normalised to 0777 (FreeBSD contract) regardless of what the
  # host lstat returns. ls used to display symlinks as regular
  # files because both pieces were broken.
  'test_symlink_lstat_perms',
  # user_from_uid + group_from_gid must use SEPARATE wasm scratch
  # buffers. ls's `printf("%s %s", uname, gname)` evaluated both
  # before printf; pre-fix they wrote into one shared buffer and
  # ls displayed `staff staff` for every file.
  'test_pwd_user_group_distinct',
  # getline(): reads lines from a FILE* — used by ssh's config
  # parser. Was stubbed -ENOSYS; ssh got zero lines from
  # ~/.ssh/config and tried to DNS-resolve every Host alias.
  'test_getline_basic',
  # ssh libc-surface pins. The ytrace of `ssh nixem` shows 47 distinct
  # libc imports; the entries below cover the non-trivial ones (BSD
  # string ops, secret scrubbing, address resolution, host/services
  # DB, randomness, set-uid detection). The pure mem/str passthroughs
  # are covered by test_libc_coverage and test_write_passthrough.
  'test_ssh_explicit_bzero',
  'test_ssh_str_bsd',
  'test_ssh_arc4random_buf',
  'test_ssh_getaddrinfo_localhost',
  'test_ssh_gethostname',
  'test_ssh_issetugid',
  'test_ssh_getservbyname',
  'test_ssh_ppoll',
  # /dev/null pollability — darwin returns POLLNVAL where Linux
  # returns POLLIN. ssh dups stdin and polls it; when stdin is
  # /dev/null (cron, non-interactive shells, `ssh host cmd </dev/null`),
  # the POLLNVAL trips ssh's fatal "invalid rfd pollfd[N]" check.
  # yos_poll detects regular-file / /dev/null fds via fstat and
  # synthesises POLLIN|POLLOUT.
  'test_ssh_poll_dev_null',
  # dup/dup2/close cycle ssh runs against stdio when setting up its
  # session channel. A bug where close on the source kills the dup
  # poisons every subsequent write to the session.
  'test_ssh_dup_stdio',
  # socket → bind → listen → connect → accept → getsockname /
  # getpeername / setsockopt / getsockopt. The same dance ssh runs
  # to set up its TCP connection. Pins the darwin sockaddr family
  # byte position (sa_family @ byte 1, not @ uint16 little-endian
  # @0/1 like Linux).
  'test_ssh_socket_loopback',
  # sigaction + sigprocmask round-trip on the FreeBSD 16-byte
  # sigset_t. ssh wraps every channel-loop iteration in these.
  'test_ssh_sigaction_mask',
  # atoi / timingsafe_bcmp / memcmp / memmove / memchr / umask /
  # getpid / getuid / time / clock_gettime / setlocale.
  'test_ssh_misc_libc',
  # strlen/strcmp/strncmp/str*casecmp/strchr/strrchr/strcpy.
  'test_ssh_string_basic',
  # malloc/calloc/realloc/free contracts ssh leans on for channel
  # state and packet buffers.
  'test_ssh_alloc_basic',
  # readpassphrase(3) — BSD libc, not in glibc, missed by the darwin
  # host-API extractor. ssh / sshd / ssh-add prompt for key passphrases
  # through this and trap with "unresolved import env.readpassphrase"
  # without the hand bridge.
  'test_ssh_readpassphrase',
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
  # -O2 is REQUIRED here. Without it, wasm-opt's asyncify pass emits
  # a `_start` whose locals overflow wasm3's compile-time function
  # stack-height limit, and the host bails out with "compiling
  # function overran its stack height limit" before even calling
  # main. -O2 shrinks the asyncify-instrumented frame enough that
  # _start fits. The other (smaller) libc tests get away without
  # -O2 because their _start frame is already compact.
  command          : [wasm_opt, '--asyncify', '-O2', '-o', '@OUTPUT@', '@INPUT@'],
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

# Comprehensive libc-coverage smoke test. One wasm program calls
# every libc function we can probe safely (touches only /tmp,
# 127.0.0.1 loopback, in-process pthread/mutex). Each call emits
# `FN:<name>:PASS|FAIL|SKIP`. The Python runner parses the output,
# cross-checks against the bridged-function list extracted from the
# generated yos_bridge.h, and writes a markdown coverage table to
# tmp/libc-coverage.md. Per-function failures show up in the table
# but DO NOT fail this test — the goal is one-shot visibility into
# which bridges work and which don't, so we don't have to fix them
# one-at-a-time as nvim/zsh/ssh tripping over each. The test only
# fails if the wasm aborts before printing COVERAGE-DONE.
libc_coverage_raw = custom_target(
  'test_libc_coverage-raw.wasm',
  input            : files('test_libc_coverage.c'),
  output           : 'test_libc_coverage-raw.wasm',
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
                      '-Wl,--export-all',
                      '-O2',
                      sysroot_dir / 'usr' / 'lib' / 'crt1.o',
                      '-o', '@OUTPUT@', '@INPUT0@'],
  build_by_default : true,
)
test('test_libc_coverage',
  py,
  args : [
    files('run_libc_coverage.py'),
    '--yos',       yos_bin,
    '--src',       files('test_libc_coverage.c'),
    '--wasm',      libc_coverage_raw,
    '--repo',      meson.project_source_root(),
    '--build-dir', meson.project_build_root().split('/')[-1],
    '--timeout',   '60',
  ],
  suite   : 'libc',
  timeout : 90,
)

# Tests in this set are known-failing pins for open bugs. The test
# still RUNS — when the underlying bug is fixed, the suite reports
# UNEXPECTEDPASS and the entry should be removed.
#
# `test_post_fork_stderr_clean` was in this list. Fixed by moving
# the per-fd F_DUPFD loop from the forked child thread into
# yos_fork_pump (parent thread, before resume) so the snapshot
# captures stable host fds rather than racing fd-table slots
# against the parent's post-fork close/reuse traffic. See
# src/yos/impl/proc.c diff.
libc_tests_xfail = [
]

foreach t : libc_tests
  src = files(t + '.c')

  # Tests that include the auto-generated yos_imports.h need the
  # codegen output dir on their include path AND a dep on the bridge
  # custom_target so meson regenerates the header before compiling.
  extra_inputs = []
  extra_cflags = []
  if t in ['test_generated_imports', 'test_fork_basic',
           'test_pthread_create_join']
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
  needs_asyncify = t in ['test_fork_basic',
                         'test_fork_channel_from_stdio',
                         'test_post_fork_stderr_clean']

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
    suite       : 'libc',
    should_fail : t in libc_tests_xfail,
  )
endforeach
