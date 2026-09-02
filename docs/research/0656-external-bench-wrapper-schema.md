# Research-0656: external-bench fork wrapper schema mismatch

- **Status**: Active
- **Workstream**: external benchmark harness robustness
- **Last updated**: 2026-05-20

## Question

Why can the fork's own `external-bench` competitors disappear from reports
even though their wrappers run successfully?

## Sources

- [`tools/external-bench/compare.py`](../../tools/external-bench/compare.py)
  validates `summary.competitor` against the registry key passed to
  `run_wrapper()`.
- [`tools/external-bench/fork-fr-regressor/run.sh`](../../tools/external-bench/fork-fr-regressor/run.sh)
  rewrites `vmaf-tune predict` output for the in-tree full-reference
  regressor.
- [`tools/external-bench/fork-nr-metric/run.sh`](../../tools/external-bench/fork-nr-metric/run.sh)
  rewrites `vmaf-tune predict` output for the in-tree no-reference metric.
- [`tools/external-bench/tests/test_compare.py`](../../tools/external-bench/tests/test_compare.py)
  previously tested `run_wrapper()` with a Python stub, but did not execute
  the fork shell wrappers themselves.

## Findings

The wrapper registry keys are `fork-fr-regressor` and `fork-nr-metric`.
Those are the values accepted by `--competitors`, displayed in the sample
table, and expected by `validate_wrapper_output()`.

The fork wrappers emitted `fork-fr-regressor-v2-ensemble` and
`fork-nr-metric-v1` in `summary.competitor`. That mismatch makes
`run_wrapper()` raise `RuntimeError("invalid schema")`; `main()` catches
that error and skips the `(competitor, corpus item)` pair. The report then
contains zero rows for a fork competitor even though the underlying
`vmaf-tune` invocation produced JSON.

The existing tests did not catch this because the main harness tests stubbed
`subprocess.run` and wrote already-valid payloads directly. The missing test
shape was a shell-wrapper smoke that stubs only the external executable
(`vmaf-tune`) and lets the wrapper's Python heredoc produce the final JSON.

## Decision

Use registry keys as the `summary.competitor` identity in the two fork
wrappers and add shell-wrapper smoke tests for both. The smoke uses a fake
`vmaf-tune` executable plus temporary model JSON files, so it remains
dependency-free and does not require `x264-pVMAF` or DOVER-Mobile.

## Follow-ups

- Keep model-version detail out of `summary.competitor`; add optional
  metadata if report users need the exact ONNX ID later.
- Reuse the same registry keys when NR/MOS second-opinion materialisation
  consumes external-bench wrapper output.
