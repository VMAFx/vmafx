<!-- markdownlint-disable MD013 MD060 -->
# ADR-0654: Predictor Preserves Saliency Signals

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, vmaf-tune, saliency, predictor

## Context

The per-shot predictor input contract already reserves five perceptual
signal slots beyond bitrate and geometry: `saliency_mean`,
`saliency_var`, `frame_diff_mean`, `y_avg`, and `y_var`. The runtime
extractor advertised `--use-saliency`, but its saliency hook called the
ROI saliency helper with a stale signature, so the optional branch
silently returned zeros. The trainer also zero-filled the same columns
even when a refreshed corpus row already carried those values.

That left the signal-mix audit's saliency / ROI gap open despite having
trained saliency weights and predictor columns in tree.

## Decision

`vmaf-tune predict --use-saliency` will decode the current shot range to
temporary `yuv420p`, run the configured `saliency_student` ONNX through
the existing `vmaftune.saliency.compute_saliency_map(...)` helper, and
feed the resulting mean and variance into `ShotFeatures`. Predictor
training will preserve row-provided probe-byte, saliency, and
signalstats columns whenever present, falling back to the historical
bitrate stand-ins and zeros only for legacy rows.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep zero-filling saliency and signalstats in trainer rows | Maximum backward compatibility and no retraining drift from richer rows | Discards the exact signals the predictor input schema already reserved, so refreshed corpora cannot close the saliency gap | Rejected because it preserves the bug-shaped blind spot. |
| Add a new wider predictor input schema | Cleanly distinguishes old and rich corpora | Invalidates every shipped predictor ONNX and sidecar vector layout | Too much blast radius for a bug fix; the existing 14-column layout already has the required slots. |
| Decode shot ranges before saliency inference | Works for any FFmpeg-readable `predict --source` and reuses the proven raw-YUV saliency helper | Adds an optional ffmpeg decode per sampled shot when `--use-saliency` is enabled | Chosen because the CLI advertises container inputs and the helper expects raw `yuv420p`. |

## Consequences

- **Positive**: Runtime `--use-saliency` now contributes real
  saliency features instead of silently falling back to zeros, and real
  predictor corpora can train from preserved saliency / signalstats
  columns.
- **Negative**: `predict --use-saliency` pays a temporary raw decode and
  saliency pass per sampled shot. The flag remains opt-in.
- **Neutral / follow-ups**: Future corpus emitters can materialise the
  same columns directly so predictor training does not need to recompute
  saliency during model refresh.

## References

- [Research-0091](../research/0091-partial-integration-audit-2026-05-08.md)
  identified the predictor saliency slots as wired but underused.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) records the
  saliency-student model contract.
- Source: req — "well that means do u2netp (experimental)... we can only learn i guess lol"
- Source: req — "well that sounds like a lot to do... go on then"
