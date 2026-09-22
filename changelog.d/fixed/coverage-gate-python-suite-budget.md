- The Coverage Gate's Python suite now gets a budget it fits in: 55 minutes
  instead of 20, with the job raised to 75. The 20-minute cap was never large
  enough — the step used to end in `|| true`, so `timeout` killing pytest
  looked like success. Master's own last full run was cut off at 64% of 711
  tests and this branch's at 63%, both silently; removing `|| true` and
  asserting the step's outcome turned that into the failure it always was.
  The new figure is measured, not guessed: exact per-file costs from the
  killed run for the 448 tests that executed, plus the remaining 263 timed
  locally and scaled by the 12.8x factor the two runs share on
  `feature_extractor_test.py`, giving ~42 minutes for the whole suite.
  `pytest-timeout=180` per test is still the anti-hang gate; the outer
  `timeout` only keeps a wedged subprocess from eating the job budget before
  gcovr runs.
