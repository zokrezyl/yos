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
  'script-mode'     : {'src' : files('test_script_mode.py'),     'xfail' : false},
  # Pinned regression for the runhookdef indirect-call-type-mismatch
  # trap (root cause: invalidate_list / accept_last registered as
  # Hookfn with the wrong wasm signature). Patched in
  # build-tools/wasm-pkg/configs/zsh/build.sh — keep this test green
  # so the trampolines aren't accidentally dropped.
  'runhookdef-trap' : {'src' : files('test_runhookdef_trap.py'), 'xfail' : false},
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
