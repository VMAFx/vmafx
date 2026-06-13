# Research-0117: vmaf-roi-score synthetic mask materialisation

- **Status**: Active
- **Workstream**: ROI scoring smoke path
- **Last updated**: 2026-05-14

## Question

Should `vmaf-roi-score --synthetic-mask` remain a pure subprocess smoke
that scores the original distorted YUV twice, or should it exercise the
same mask-materialisation path as `--saliency-model` without running
ONNX inference?

## Sources

- [`tools/vmaf-roi-score/src/vmafroiscore/cli.py`](../../tools/vmaf-roi-score/src/vmafroiscore/cli.py)
  wired `--synthetic-mask` to re-score the unmodified distorted YUV,
  so the fill value never affected the masked score.
- [`tools/vmaf-roi-score/src/vmafroiscore/mask.py`](../../tools/vmaf-roi-score/src/vmafroiscore/mask.py)
  already exposes an injectable `inference` callable and a
  `synthesise_uniform_mask()` helper.
- [`docs/usage/vmaf-roi-score.md`](../usage/vmaf-roi-score.md)
  presents synthetic mode as a mask smoke surface.

## Findings

The existing mask materialiser already has the right test seam:
`apply_saliency_mask()` accepts an inference callable returning a
height-by-width mask. Synthetic mode can supply a constant callable and
therefore avoid ONNX Runtime while still validating YUV frame sizing,
plane splitting, threshold/fade conversion, chroma downsampling, output
materialisation, the second `vmaf` subprocess call, and final score
blending.

## Alternatives considered

Keeping the old behavior was rejected because it made
`--synthetic-mask` a misleading option: every fill value produced the
same second input as the full-frame run, so it could not catch mask
pipeline regressions.

Adding a separate pure-Python blend-only smoke command was rejected
because the project already has unit coverage for `blend_scores()`.
The useful operator smoke is the one that catches file/materialiser
regressions without requiring an ONNX model.

## Decision

Route `--synthetic-mask FILL` through `apply_saliency_mask()` with a
constant synthetic inference callback. Keep `--threshold` and `--fade`
semantics identical to the real saliency path so synthetic mode is a
real materialiser smoke, not a second masking dialect.
