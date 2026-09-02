## Fixed

- **Perf gate (ADR-0907)**: the `check-regression.py` step was always exiting
  non-zero because the committed baseline was recorded on a developer
  workstation that is 5–15x faster than GitHub Actions runners (ADR-1005).
  Added `--advisory` (exit 0, full report still printed) and
  `--skip-if-no-baseline` (skip cleanly when no comparable baseline data
  exists) flags to `scripts/perf/check-regression.py`.  The CI workflow now
  passes `--advisory --skip-if-no-baseline` until a CI-runner-calibrated
  baseline is committed.  Added `docs/development/perf-gate.md` with the
  baseline refresh procedure.
