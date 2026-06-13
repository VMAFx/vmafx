- **AI / DNN:** Replaced the `transnet_v2` smoke-only placeholder ONNX
  with real upstream TransNet V2 weights (Soucek & Lokoc 2020; MIT
  license) pinned at `soCzech/TransNetV2` commit `77498b8e`. The
  exporter wraps upstream's NTHWC `[1, 100, 27, 48, 3]` SavedModel in
  a 4-line `tf.Module` adapter that transposes inputs from the C-side
  NTCHW `[1, 100, 3, 27, 48]` contract (ADR-0223) and selects only
  the single-frame logits output (squeezed to `[1, 100]`). One rank-2
  `UnsortedSegmentSum` in upstream's `ColorHistograms` branch is
  rewritten as an equivalent `ScatterND` reduction='add' subgraph for
  ONNX opset-17 compatibility; six standard ONNX ops join the libvmaf
  op allowlist (`BitShift`, `GatherND`, `Pad`, `Reciprocal`,
  `ReduceProd`, `ScatterND`). Registry row `model/tiny/registry.json`
  flips `smoke: false` with the MIT license, upstream commit pin, and
  refreshed sha256. ~30 MiB ONNX, opset 17. TF SavedModel parity:
  max-abs-diff `< 4e-6` across 3 random `[0..255]` trials. New
  exporter `ai/scripts/export_transnet_v2.py` (placeholder kept for
  reference). See ADR-0257 and `docs/ai/models/transnet_v2.md`.
