- **Tests: the live window harness's 33 ms latency budget runs in its own
  Meson suite, `timing`.** The budget (a window over per-frame features
  completes within two frame periods of its last frame's submit) is a
  wall-clock check that failed under host load. `test_vmafx_window_live_timing`
  in the `timing` suite asserts it, one test at a time on an idle host
  (`make test-timing`). The `fast` suite runs the same harness with every
  other check, and the new `test_vmafx_live_pacing` checks the harness's
  pacing and budget arithmetic exactly on a virtual clock, refusing two
  planted pacing bugs. The budget is unchanged
  ([test suites](docs/development/test-suites.md)).
