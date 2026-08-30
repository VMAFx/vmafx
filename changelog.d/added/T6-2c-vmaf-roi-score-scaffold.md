- **`tools/vmaf-roi-score/` — region-of-interest VMAF scoring scaffold
  (ADR-0288 Option C Accepted, Research-0063).** New Python tool that
  drives the `vmaf` CLI twice (full-frame + saliency-masked) and
  blends the two pooled scalars via a user-controlled weight
  `w ∈ [0, 1]`: `roi_vmaf = (1 - w) * vmaf_full + w * vmaf_masked`.
  Distinct from the existing `core/tools/vmaf_roi.c` binary
  (ADR-0247) — that surface emits encoder QP-offset sidecars; this
  one produces a saliency-weighted score. Combine math
  (`vmafroiscore.blend_scores`), CLI surface (`--reference /
  --distorted / --weight / --synthetic-mask / --saliency-model`),
  JSON output schema (`SCHEMA_VERSION = 1`, key order pinned via
  `ROI_RESULT_KEYS`), and the `vmaf` subprocess seam ship in this
  PR. The `--saliency-model` ONNX inference path is wired and
  validated but mask materialisation deliberately exits 64 — gated
  on PR #359 (`saliency_student_v1`) merging and a follow-up PR
  for the YUV reader/writer + ORT pre/post-proc loop. Subprocess-
  mocked smoke tests under `tools/vmaf-roi-score/tests/` (14 cases)
  pin the combine math endpoints + midpoint, the JSON schema key
  order, the synthetic-mask end-to-end path, and the deferred
  `--saliency-model` exit-code contract. **Option A** (per-pixel
  feature pooling weighted by saliency in libvmaf C code) is
  explicitly deferred to a separate ADR — this scaffold avoids the
  Netflix golden gate and cross-backend ULP-diff burden entirely.
  **No MOS-correlation claim** is made; validation against a
  labelled saliency-MOS corpus is research follow-up. User docs:
  [`docs/usage/vmaf-roi-score.md`](../docs/usage/vmaf-roi-score.md).
