- **`vmaf-tune` saliency-aware ROI tuning — Bucket #2 of the PR #354
  audit (ADR-0293).** New `tools/vmaf-tune/src/vmaftune/saliency.py`
  consumes the fork-trained `saliency_student_v1` ONNX
  (ADR-0286 / PR #359) to produce a per-MB QP-offset map; new
  `vmaf-tune recommend --saliency-aware` subcommand wires it into
  the FFmpeg encode path via x264 `--qpfile`. Saliency map is the
  per-pixel mean across `--saliency-frames` evenly-spaced sampled
  frames (default 8); `--saliency-offset` (default `-4`) is the
  QP delta at peak saliency, clamped to ±12 to match `vmaf-roi`'s
  ADR-0247 sidecar convention. Pure-Python ONNX inference (mocked
  in the test suite via `session_factory=…`) so the harness ships
  without a built libvmaf dependency; graceful fallback to plain
  encode when `onnxruntime` or the model file is unavailable.
  13 new unit tests under
  [`tools/vmaf-tune/tests/test_saliency.py`](../tools/vmaf-tune/tests/test_saliency.py)
  cover the qp-map signal blend (saliency=1.0 → −4, saliency=0.0
  → +4, saliency=0.5 → 0, clamped to ±12), per-MB block reduce,
  x264 qpfile emission, end-to-end encode-command shape, and the
  unavailable-fallback path. x264 only in this PR; x265 / SVT-AV1
  inherit `vmaf-roi`'s C sidecar today and become a one-file
  codec-adapter follow-up. User docs:
  [`docs/usage/vmaf-tune.md` §"Saliency-aware encoding"](../docs/usage/vmaf-tune.md).
