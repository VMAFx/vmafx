- **The two code-scanning gates failed on findings that were real, so the code
  changed rather than the alerts.** `core/test/test_windows_cuda_compiler_discovery.py`
  took the Meson to configure its fixture project from a `VMAFX_TEST_MESON`
  environment variable that `core/test/meson.build` set, and fed it straight
  into a `subprocess` argv — the taint flow Semgrep's
  `dangerous-subprocess-use-tainted-env-args` reports. The test now asks its own
  interpreter for the `mesonbuild` package (falling back to a `PATH` lookup),
  which under `meson test` *is* the Meson running the suite, so the hand-off and
  the environment variable are both gone. Passing the path as a test argument
  instead would not have helped: the rule treats `sys.argv` as a source too.
  `core/test/test_pelorus_interop.c` created its x265 CSV fixture with a bare
  `fopen(path, "w")`, i.e. mode 0666 before umask — measured world-writable
  under a permissive umask — which CodeQL rates high
  (`cpp/world-writable-file-creation`); it now opens a descriptor with an
  explicit 0600 and wraps it, the same shape `write_backend_error_json()` in
  `core/tools/vmaf.cpp` already used. `_discover_frame_features()` in
  `compat/python-vmaf/core/quality_runner.py` assigned its hit flag one last
  time with no reader left (CodeQL `py/multiple-definition`); the call stays for
  its side effects and the dead store is gone. No alert was dismissed, no rule
  was disabled, and the stale `nosemgrep` directive the finding used to carry
  was removed rather than repositioned.
