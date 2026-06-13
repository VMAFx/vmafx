- **ONNX op-allowlist gains `Resize` (ADR-0258 / T7-32).**
  One-line addition under `core/src/dnn/op_allowlist.c`'s
  `/* convolutional */` block unblocks U-2-Net (PR #341 follow-up)
  and the wider saliency / segmentation surface — mobilesal,
  BASNet, PiDiNet, FPN-style detectors all rely on `Resize` for
  decoder-side spatial upsampling. The wire-format scanner stays
  op-type-only per ADR D39 / ADR-0169; consumers shipping their
  own ONNX should keep `mode in ("nearest", "linear")` (`cubic`
  is numerically less stable on quantised inputs and not exercised
  by any in-tree consumer). Python `vmaf_train.op_allowlist`
  regex parser surfaces the new entry automatically — export-time
  + load-time symmetry preserved. New tests:
  `test_resize_op_allowed` (C allowlist),
  `test_resize_top_level_allowed` (C wire-format scan),
  `test_resize_now_allowed` (Python parser). 47/47 libvmaf tests
  + 15/15 Python tests green. See
  [ADR-0258](docs/adr/0258-onnx-allowlist-resize.md) +
  [`docs/ai/security.md`](docs/ai/security.md).
