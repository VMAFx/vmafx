- Tests and testdata tools: the fork-licensed Python files under `python/` and
  `testdata/` now run every process through `scripts/lib/safe_subprocess.run`,
  so the master run of the `check-fork-python-subprocess` hook passes again
  (42 findings in 17 files). A new case in
  `scripts/ci/tests/test_check_fork_python_subprocess.py` runs the check over
  the whole tree, so a pull request that only touches other files can no
  longer leave master red. The no-zimg build now expects the
  pre-converted ICtCp rows of `python/test/hdr_models_pin_test.py` to score,
  since they need no zimg conversion.
