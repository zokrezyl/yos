# tests/integration/zsh — drive the wasm-built zsh under yos.
#
# These tests assume `nix build .#zsh` has already been run (the
# discovery code looks up the result via `nix path-info .#zsh`).
# If that fails, tests print SKIP and exit 0 — non-fatal so meson
# can still build clean on a checkout that hasn't built zsh yet.
#
# Pass YOS_ZSH_WASM=/abs/path/to/zsh.wasm to override discovery
# (useful when iterating without going through nix).

py3 = import('python').find_installation('python3')

zsh_int_env = environment()
zsh_int_env.set('YOS_REPO_ROOT', meson.project_source_root())
zsh_build_basename = run_command('basename', meson.project_build_root(),
                                 check : true).stdout().strip()
zsh_int_env.set('YOS_BUILD_DIR', zsh_build_basename)

zsh_int_tests = {
  'script-mode'      : {'src' : files('test_script_mode.py'),      'xfail' : false},
  'grammar'          : {'src' : files('test_grammar.py'),          'xfail' : false},
  'env-inherit'      : {'src' : files('test_env_inherit.py'),      'xfail' : false},
  'external-missing' : {'src' : files('test_external_missing.py'), 'xfail' : false},
  # Pinned regression for the runhookdef indirect-call-type-mismatch
  # trap (root cause: invalidate_list / accept_last registered as
  # Hookfn with the wrong wasm signature). Patched in
  # build-tools/wasm-pkg/configs/zsh/build.sh — keep this test green
  # so the trampolines aren't accidentally dropped.
  'runhookdef-trap'  : {'src' : files('test_runhookdef_trap.py'),  'xfail' : false},
  # Tests that used to be xfail and were promoted as their underlying
  # bug got fixed. Kept as regular passing tests so the regression net
  # catches the next time someone touches the relevant code path.
  #   silent-builtins      → fixed: nixpkgs/sysroot/default.nix now
  #                           defines __stdinp/__stdoutp/__stderrp as
  #                           the small-int handles 1/2/3 the bridge
  #                           understands. Without those, every
  #                           fputs/fwrite/printf got a NULL host FILE*
  #                           and silently dropped.
  #   external-diagnostic  → fixed alongside silent-builtins (same root
  #                           cause: zsh's "command not found" text is
  #                           emitted via stderr stdio).
  #   env-passthrough      → fixed: build-tools/sysroot/skel.sh's crt1
  #                           now calls __yos_envc + __yos_envp_setup,
  #                           defines `char **environ` as a static
  #                           backing array of 256 entries, and
  #                           populates it before main(). Without this
  #                           zsh's createparamtable() walked a NULL
  #                           environ and never imported PATH/HOME.
  'silent-builtins'     : {'src' : files('test_silent_builtins.py'),     'xfail' : false},
  'external-diagnostic' : {'src' : files('test_external_diagnostic.py'), 'xfail' : false},
  'env-passthrough'     : {'src' : files('test_env_passthrough.py'),     'xfail' : false},
  # User-reported "backspace = space" bug. ZLE emits the right
  # `\b \b` sequence; the visible corruption was caused by a yos
  # fork-asyncify leak — a forked child wrote 8 bytes of 0xff to
  # the dup'd tty fd, modern UTF-8 terminals rendered those as
  # replacement glyphs, and the destructive-backspace's column
  # accounting fell apart. Workaround in vfs.c::yos_write +
  # file.c::yos_fwrite drops short pure-0xff writes going to a
  # tty (isatty(hfd)==1). The test asserts NO 0xff garbage in the
  # post-backspace byte stream. The deeper proc.c fork-snapshot
  # bug is still open; YOS_NO_FF_DROP=1 disables the workaround.
  'backspace'           : {'src' : files('test_backspace.py'),           'xfail' : false},
  # Bug pins still standing (xfail). Each flips to UNEXPECTEDPASS when
  # fixed — remove the xfail entry as part of the fix commit.
  #
  # NOTE on pipe-hang / cmdsubst-hang:  The chain of fixes landed
  # in this round (wasm-opt --asyncify on zsh, SIGCHLD synchronously
  # invoked from sigsuspend, waitpid/wait3/kill returning -1+errno,
  # process-wide signal filtering, child fd cleanup on pthread_exit,
  # stdio routing through fd_map) make these forward-progress: zsh
  # reaches the right control flow, the parent reads bytes, the child
  # exits cleanly. But pipe-hang passes only ~75% of the time (data
  # arrives but sometimes garbled with 0xFF prefix); cmdsubst-hang
  # delivers "inner" to host stdout instead of the captured pipe. Both
  # symptoms point at the asyncify-rewind / fork fd-dup interaction
  # for fds reallocated via movefd. xfail until that's tracked down.
  #   pipe-hang            → mostly fixed but flaky.
  #   cmdsubst-hang        → x=$(echo inner) doesn't capture; child's
  #                           output escapes to host stdout.
  #   ctrl-c-cancel        → Ctrl-C at the prompt kills zsh instead of
  #                           cancelling the line (yos signal/asyncify).
  'pipe-hang'           : {'src' : files('test_pipe_hang.py'),           'xfail' : true},
  'cmdsubst-hang'       : {'src' : files('test_cmdsubst_hang.py'),       'xfail' : true},
  'ctrl-c-cancel'       : {'src' : files('test_ctrl_c_cancel.py'),       'xfail' : true},
}

foreach name, cfg : zsh_int_tests
  test('zsh-' + name,
       py3,
       args : cfg['src'],
       suite : 'integration-zsh',
       env : zsh_int_env,
       depends : [yos_exe],
       should_fail : cfg['xfail'],
       timeout : 30)
endforeach
