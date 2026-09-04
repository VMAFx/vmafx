- **docs**: `docs/ai/quantization.md` now states the int8 **wire format**
  explicitly. The page previously never mentioned QOperator and documented no
  format for any shipped model, so a reader could not tell what the fork emits,
  what it loads, or what happens when an int8 file is rejected. It now records
  that every shipped `.int8.onnx` is QOperator-dynamic
  (`DynamicQuantizeLinear` + `MatMulInteger` / `ConvInteger`, zero QDQ nodes),
  that the op allowlist accepts QDQ and QOperator-dynamic but rejects
  QOperator-static `QLinear*` ops with `-EPERM`, and that a missing or rejected
  int8 file falls back to the fp32 baseline with only a `DEBUG`-level log line
  (ADR-1032, which reversed ADR-0174 §2's hard error). Also corrects a factually
  wrong caveat that blamed the no-VNNI slowdown on "QDQ overhead" — no shipped
  model contains a `QuantizeLinear` node.
