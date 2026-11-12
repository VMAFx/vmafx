---
paths:
  - core/src/feature/feature_lpips.c
  - core/src/feature/feature_dists.c
invariant: LPIPS and DISTS tiny-AI metric contracts and high-bit-depth normalization.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# LPIPS and DISTS High-Bit-Depth Normalization

- [ADR-0024](../../../../docs/adr/0024-netflix-golden-preserved.md) —
  three CPU golden pairs never change.
- [ADR-0041](../../../../docs/adr/0041-lpips-sq-extractor.md) — LPIPS
  extractor registration pattern.
- [ADR-0042](../../../../docs/adr/0042-tinyai-docs-required-per-pr.md) —
  DNN-backed extractors ship docs under `docs/ai/`.
- [ADR-0236](../../../../docs/adr/0236-dists-extractor.md) — `dists_sq`
  mirrors LPIPS' two-input tiny-AI extractor shape. Keep
  `VMAF_DISTS_SQ_MODEL_PATH`, `model_path`, registry id
  `dists_sq_placeholder_v0`, and `score` scalar output aligned until
  real DISTS weights replace smoke checkpoint.
- **LPIPS / DISTS high-bit-depth input invariant** — both extractors
  accept planar 8/10/12/16-bit YUV but keep ONNX tensor ABI as
  ImageNet-normalised RGB8. High-bit-depth samples are little-endian
  16-bit containers rounded into 8-bit domain before shared
  BT.709 limited-range RGB conversion.
