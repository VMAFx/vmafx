# Research-0716 — vmaf-tune strict JSON artifact emitters

## Context

Research-0715 introduced `vmaftune.jsonio.dumps_strict()` for report and
compare JSON. A follow-up sweep found additional report-style tune artifacts
still using plain `json.dumps()`: bitrate-ladder descriptors, benchmark JSON,
auto-plan JSON, and split-conformal calibration sidecars.

## Findings

- `vmaf-tune auto` can carry `interval_width = NaN` when a caller supplies
  confidence intervals for some cells but not others. Plain `json.dumps()` emits
  the JavaScript token `NaN`, which strict JSON decoders reject.
- `ladder` and `benchmark` renderers are consumed by notebooks, dashboards, and
  encoder-profile follow-up tooling. They should share the same
  NaN/Infinity-to-null rule as compare and report output.
- `SplitConformalCalibration.to_json()` already validates finite residuals, but
  using the shared helper keeps the sidecar on the same strict-dump path and
  preserves compact output through `indent=None`.

## Alternatives considered

- **Leave remaining emitters alone.** Rejected because users would still see
  inconsistent strict-parser behaviour across tune subcommands.
- **Inline another `allow_nan=False` call per emitter.** Rejected because it
  repeats the portability policy without the recursive non-finite coercion.
- **Move the helper repo-wide now.** Rejected for this slice; `vmaf-tune` has
  the known report/profile JSON consumers, while other packages need separate
  schema audits before a shared utility becomes a project-wide contract.

## Smoke

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_ladder.py tools/vmaf-tune/tests/test_benchmark.py tools/vmaf-tune/tests/test_auto_confidence_aware.py tools/vmaf-tune/tests/test_conformal.py -q
```
