- **Tests: the `timing` suite exists only in a plain optimised build.**
  Coverage counters, a sanitizer or a build without optimisation slow the
  code down, so the live window harness's 33 ms budget there measured the
  instrumentation and failed the coverage job. Such a build now compiles
  the harness with the budget off and registers no `timing` test; the
  release builds still hold the budget, and `test_timing_suite_plain_builds`
  checks the rule for every build
  ([test suites](docs/development/test-suites.md)).
