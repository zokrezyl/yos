# tests/integration/nvim — black-box tests that drive nvim through a pty
# and verify behaviour via captured terminal output.
#
# These run only when the nvim wasm package is present at
# build-linux/wasm-pkgs/nvim-0.10.4/out/bin/nvim.wasm. Build it via:
#   tools/wasm-pkg.sh nvim
# and the yos host binary via the standard meson build.
#
# Each test is a self-contained python3 script under this directory; it
# uses run_in_pty.py (a thin pty.fork + script-style wrapper) to spawn
# yos+nvim with a controlled pty size, drives it with timed keystrokes,
# and exits 0 on PASS / non-zero on FAIL with diagnostic output.
#
# YOS_REPO_ROOT is passed via env so tests can locate the yos binary
# and nvim.wasm without depending on the test runner's cwd.

py3 = import('python').find_installation('python3')
nvim_wasm = meson.project_build_root() / 'wasm-pkgs' / 'nvim-0.10.4' \
                                       / 'out' / 'bin' / 'nvim.wasm'

nvim_int_env = environment()
nvim_int_env.set('YOS_REPO_ROOT', meson.project_source_root())
# Tell tests where the host yos binary lives (build dir basename, e.g.
# build-linux on linux, build-darwin on darwin). Tests join repo +
# build_dir + "src/yos/yos" — defaults to build-linux for back-compat.
yos_build_basename = run_command('basename', meson.project_build_root(),
                                 check : true).stdout().strip()
nvim_int_env.set('YOS_BUILD_DIR', yos_build_basename)

nvim_int_tests = {
  'quit'         : {'src' : files('test_quit.py'),         'xfail' : false},
  'winsize'      : {'src' : files('test_winsize.py'),      'xfail' : false},
  'edit-session' : {'src' : files('test_edit_session.py'), 'xfail' : false},
  # Pins the user-reported "no text input" symptom. Drives nvim with
  # 'iHELLO<Esc>:q!<CR>' and verifies HELLO appears in the pty output.
  'input'        : {'src' : files('test_input.py'),        'xfail' : false},
}

foreach name, cfg : nvim_int_tests
  test('nvim-' + name,
       py3,
       args : cfg['src'],
       suite : 'integration-nvim',
       env : nvim_int_env,
       depends : [yos_exe],
       should_fail : cfg['xfail'],
       timeout : 30)
endforeach
