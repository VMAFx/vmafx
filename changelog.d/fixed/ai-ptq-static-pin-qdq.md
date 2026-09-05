- **Static PTQ format pinned to QDQ**: `ai/scripts/ptq_static.py` now explicitly
  passes `quant_format=QuantFormat.QDQ` to `onnxruntime.quantization.quantize_static`,
  preventing silent emission of QOperator (`QLinear*`) graphs that libvmaf's DNN op
  allowlist (`core/src/dnn/op_allowlist.c`) rejects. Closes
  `T-AI-PTQ-STATIC-QUANT-FORMAT-UNPINNED-2026-09-03` under ADR-0129/0174 policy.
  Verified with end-to-end roundtrip test `test_ptq_static_full_roundtrip` asserting
  QDQ allowlisted ops only.
