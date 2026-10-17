## Coverage jobs: gcov timeout multiplier and named shard skips (2026-10-17)

- Every `run_meson_test.py` command of a job with a `-Db_coverage=true` build passes `--timeout-multiplier 3`; the Makefile variable is `INSTRUMENTED_TEST_TIMEOUT_MULTIPLIER` (was `SANITIZER_TEST_TIMEOUT_MULTIPLIER`) and covers `test-sanitizers` and `coverage`. `scripts/ci/tests/test_sanitizer_suite_timeouts.py` refuses a run without it.
- The coverage job's steps before the shards carry ids that the shard assertion reads through `EARLIER_OUTCOMES`; a rebase that renames or adds such a step updates that list. `scripts/ci/test_fail_closed_ci.py` checks that each id exists and that the shard steps set `PYTHONPATH`.
