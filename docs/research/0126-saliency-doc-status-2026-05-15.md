<!-- markdownlint-disable MD013 MD025 -->
# Research 0126: saliency docs status sweep

# Research-0126

## Scope

User-facing docs still described the T6-2 saliency surface as if the
only shipped artefact were the synthetic MobileSal placeholder and as
if `vmaf-roi` were still 8-bit-only. This sweep reconciles the docs
with the code and registry state already in tree.

## Findings

- `model/tiny/registry.json` keeps `mobilesal_placeholder_v0` with
  `smoke: true`, but its notes mark it superseded by
  `saliency_student_v1` for production use.
- `docs/ai/models/saliency_student_v1.md` documents shipped production
  fork-trained saliency weights using the same `input` /
  `saliency_map` tensor contract as `feature_mobilesal.c`.
- `docs/ai/models/saliency_student_v2.md` documents a higher-IoU
  resize-decoder ablation staged for later ROI A/B validation; it is
  not the production flip yet.
- `core/src/feature/feature_mobilesal.c` still rejects non-8-bit
  pictures, so high-bit-depth wording must stay scoped to `vmaf-roi`.
- `core/tools/vmaf_roi.c` and `docs/usage/vmaf-roi.md` accept
  `--bitdepth 8|10|12|16`, downscaling high-bit-depth luma into the
  8-bit DNN contract before sidecar generation.

## Decision

Update the roadmap, feature matrix, and MobileSal legacy model card
together. The docs now say:

- `mobilesal_placeholder_v0` is retained for smoke / historical ABI
  coverage.
- `saliency_student_v1` is the production saliency weight for the
  scoring-side extractor.
- `saliency_student_v2` is staged, not yet the production default.
- `vmaf-roi` supports 8/10/12/16-bit planar YUV input, while the
  scoring-side `mobilesal` feature remains 8-bit-only.

## References

- User request: "okay and another one"
- User request: "perhaps then do a docs audit and close/correct them all in one batch"
