# Research 0105 — `vmaf-tune tune-per-shot` Bisect Closure

Date: 2026-05-14

## Question

`vmaf-tune tune-per-shot` still defaulted every detected shot to the codec
adapter's default CRF. What is the smallest production wiring that retires the
CLI scaffold now that Phase-B bisect exists?

## Findings

- `per_shot.tune_per_shot()` already has the right predicate seam:
  `(Shot, target_vmaf, encoder) -> (crf, measured_or_predicted_vmaf)`.
- `bisect_target_vmaf()` is the existing real encode+score backend, but it
  expects a raw-YUV reference plus explicit geometry.
- Detected shots may come from containers or raw YUV. The CLI can isolate each
  half-open shot range with FFmpeg, then pass that temporary raw-YUV clip to
  the bisect backend.
- Keeping `--predicate-module MODULE:CALLABLE` preserves the test/operator
  escape hatch without making the adapter-default dry run the CLI default.

## Decision

Make `vmaf-tune tune-per-shot` bind Phase-B bisect by default. The CLI extracts
each shot to a temporary raw-YUV file, runs `bisect_target_vmaf()` with the
requested target, and records the measured VMAF in the JSON plan. The Python
API keeps its no-predicate dry-run fallback for programmatic smoke callers.

## Alternatives considered

- Make `tune_per_shot()` itself require a predicate. Rejected because it would
  break the library's existing dry-run API; the user-discoverable CLI is the
  production surface that needed closure.
- Inline a second bisect implementation in `per_shot.py`. Rejected because
  `bisect.py` already owns monotonicity checks, adapter validation, and
  encode/score runner seams.
- Wait for native per-codec zones/qpfiles. Rejected because segment-and-concat
  is already the documented portable emission path, and native emission is a
  codec-specific efficiency optimization.

## Validation

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest \
  tools/vmaf-tune/tests/test_per_shot.py -q
```
