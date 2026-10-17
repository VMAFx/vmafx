- CI: three causes of required jobs failing without a change to blame are removed.
  Jobs that run the Meson suite download the Go modules of its Go-running tests
  first, with retries (`scripts/ci/prefetch_go_modules.py`), so a failed download
  fails that step by name and not a unit test. Sanitizer builds run the tests with
  `--timeout-multiplier 3`; `test_vmafx_score` had taken up to 27.99 s of its 30 s
  there before one run killed it. The test-fixture downloads of
  `scripts/test/fetch-test-yuvs.sh` and of the Python harness retry a failing host
  for 275 s instead of ten seconds or not at all, and the script accepts fixture
  names to fetch only those.
