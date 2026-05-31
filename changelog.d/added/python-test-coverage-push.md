- **Python harness coverage push — round 2.** New focused unit-test
  file `python/test/python_harness_coverage_test.py` (82 cases) lifts
  coverage on pure-Python utility surfaces under `compat/python-vmaf/`
  that were previously untested. Builds on PR #412
  (`test/compat-python-vmaf-coverage`) by covering the next-tier
  modules: `__init__.py` 24% → 67%, `core/mixin.py` 70% → 96%,
  `tools/kimchi.py` 0% → 75%, `tools/misc.py` 34% → 59%,
  `tools/scanf.py` 24% → 55%, `tools/sigproc.py` 46% → 63%,
  `tools/testutils.py` 0% → 53%. Strict scope discipline: no Netflix
  golden `assertAlmostEqual` modified (CLAUDE.md §8); no subprocess
  shell-out to the `vmaf` binary — `ExternalProgramCaller.call_vmafexec`
  command-line builder is covered via `mock.patch("vmaf.run_process")`
  + `mock.patch("vmaf.required")` instead. Surfaces two pre-existing
  bugs (flagged, not patched, to keep PR coverage-only): (1)
  `tools/scanf.py:648` `makeFormattedHandler.applyWidth` has an
  inverted `if width is None` check that breaks both implicit-width
  (`%d`) and explicit-width (`%1d`) paths — only literal-delimited
  width patterns (`frame%08d.icpf`) survive; (2) `__init__.py`
  `ProcessRunner.run` uses `setdefault` for `LC_ALL`/`LANG` so a
  caller env that already has a non-C `LANG` is preserved, contrary
  to the comment claim that error messages are forced to English.
