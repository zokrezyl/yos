# tests/integration/freebsd-tools — drive the wasm-built FreeBSD
# coreutils second-batch ports under yos.
#
# These tests prove (or honestly xfail) what we built in
# nixpkgs/pkgs/freebsd-tools/. Each test follows the same shape:
#   1. discover yos + libexec via _tools_path.find_yos_and_tools
#      (no nix builds — read-only lookup).
#   2. run the tool with a small realistic input.
#   3. compare stdout/stderr/rc against either the happy-path expectation
#      or the documented failure mode.
#
# An xfail test that flips to UNEXPECTEDPASS means the runtime gap is
# fixed — drop the xfail in the same commit as the fix.
#
# Discovery requires a recent `nix build .#all` (yields the umbrella
# tree with both bin/yos and libexec/<tool>) or two separate
# `nix build .#yos` + `nix build .#freebsd-tools` invocations.

py3 = import('python').find_installation('python3')

ftools_int_env = environment()
ftools_int_env.set('YOS_REPO_ROOT', meson.project_source_root())

# Each entry: name → { src, xfail }. xfail captures the runtime
# gap that's currently blocking — see the docstring at the top of
# each test_*.py for the specific failure mode + suggested fix.
ftools_int_tests = {
  'wc'    : {'src' : files('test_wc.py'),    'xfail' : false},
  'df'    : {'src' : files('test_df.py'),    'xfail' : false},
  'awk'   : {'src' : files('test_awk.py'),   'xfail' : true},
  'cut'   : {'src' : files('test_cut.py'),   'xfail' : true},
  'tr'    : {'src' : files('test_tr.py'),    'xfail' : true},
  'grep'  : {'src' : files('test_grep.py'),  'xfail' : true},
  'sed'   : {'src' : files('test_sed.py'),   'xfail' : true},
  'sort'  : {'src' : files('test_sort.py'),  'xfail' : true},
  'xargs' : {'src' : files('test_xargs.py'), 'xfail' : true},
  'find'  : {'src' : files('test_find.py'),  'xfail' : true},
  'du'    : {'src' : files('test_du.py'),    'xfail' : true},
}

foreach name, cfg : ftools_int_tests
  test('freebsd-tool-' + name,
       py3,
       args : cfg['src'],
       suite : 'integration-freebsd-tools',
       env : ftools_int_env,
       depends : [yos_exe],
       should_fail : cfg['xfail'],
       timeout : 30)
endforeach
