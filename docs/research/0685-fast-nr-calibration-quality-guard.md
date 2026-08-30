# Research 0685: Fast-NR calibration quality guard

## Question

What should happen when `nr_metric_v1` calibration produces a sidecar that is
not predictive enough to accelerate `vmaf-tune` safely?

## Findings

- The 2026-05-21 full CPU calibration run completed, but the fitted NR-to-FR
  relationship was weak: `vmaf_fr ~= 6.6479 * vmaf_nr + 60.9433`,
  `PLCC=0.0821`, `sigma=16.2445`, and `delta_fast=32.49`.
- A large `delta_fast` would avoid many bad early eliminations, but it also
  removes most of the intended FR-call savings. The low PLCC is the stronger
  signal: the proxy is not ranking this corpus usefully yet.
- The dangerous boundary is the JSON sidecar write. Once
  `model/tiny/nr_metric_v1.json` contains `calibration_threshold`,
  `NRProxyBackend` consumes it automatically on the next `--fast-nr` run.
- One-clip smoke probes are still useful for checking tool paths, but they must
  not be indistinguishable from production calibration evidence.

## Decision

Add a quality gate to `calibrate_nr_threshold.py`: minimum sample count,
minimum positive PLCC, explicit weak-fit override, and report/sidecar metadata
that records the gate result.

## Commands

```bash
rg -n "calibration_quality|min-plcc|min-calibration-samples" ai/scripts/calibrate_nr_threshold.py ai/tests/test_calibrate_nr_threshold.py docs/usage/vmaf-tune-fast-nr.md
.venv/bin/python -m pytest ai/tests/test_calibrate_nr_threshold.py -q
```
