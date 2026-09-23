**Pin `QuantFormat.QDQ` in `ai/train/qat.py` static quantize path** (ADR-0207, Research-2029, #1242)

- `ai/train/qat.py` (`_ort_static_quantize`) now explicitly passes `quant_format=QuantFormat.QDQ`
  to `onnxruntime.quantization.quantize_static`, guaranteeing that QAT static export cannot emit
  QOperator-static ops (`QLinear*`, `QGemm`) rejected with `-EPERM` by `core/src/dnn/op_allowlist.c`.
- Added regression tests in `ai/tests/test_qat_smoke.py` (`test_qat_quantize_static_pins_qdq` and
  ONNX graph node verification in `test_qat_run_smoke`).
