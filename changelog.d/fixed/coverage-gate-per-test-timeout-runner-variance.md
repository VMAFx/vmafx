- **`Coverage Gate` killed a healthy test because its per-test timeout
  was a point estimate, not a range.** The step's
  `pytest-timeout --timeout=180` was sized off one observation of one
  test (`vifks360o97` at about 138 s in a `--buildtype=debug
  -Db_coverage=true` build). Hosted-runner throughput varies far more
  than that: reconstructing per-test wall clocks from the `-v` output of
  two Coverage Gate jobs on the same recipe, every vmaf-CLI-bound test
  in `quality_runner_test.py` ran 1.51-1.55x slower in the second job,
  the `meson test` step in the same job ran 3.9x slower (30 s against
  118 s), and `test_run_vmaf_runner_float_vifks2` went from 78.4 s to
  over 180 s — at least 2.30x. Because the step uses
  `--timeout-method=thread`, pytest-timeout does not fail just the slow
  test: it dumps stacks and calls `os._exit(1)`, so the session aborted
  at 60% of 711 tests and the step reported a failure with no assertion
  behind it. The per-test budget is now 600 s, sized as the slowest
  measured test (142.7 s) times the worst measured runner inflation
  (2.30x) = 328 s, with 1.8x headroom, and still only 18% of the outer
  55-minute backstop so a genuinely wedged test fails as a test rather
  than killing the step. No test was deselected, marked slow, or
  otherwise removed from the measurement.
