- **Research digest** — int8 static PTQ / QAT readiness smoke for the 1.0.0 retrain
  (`docs/research/2029-int8-static-qat-readiness.md`): CPU-only end-to-end smoke of
  `ai/scripts/ptq_static.py` and `ai/scripts/qat_train.py` against the Netflix 5-frame
  pair, the per-path operator inventory versus `core/src/dnn/op_allowlist.c` (all QDQ
  and dynamic-QOperator ops already allowlisted; issue #1242's premise is stale), the
  `vmaf --tiny-model` loader status, fp32-vs-int8 drop measurements, a recommended
  two-tier drop gate, and the gap list for epic #1242. Opens two `docs/state.md` rows:
  the missing `.int8.onnx` redirect in `vmaf_use_tiny_model()` and the missing
  `onnx_has_scaler` flag in `model/tiny/vmaf_tiny_v3.int8.json` (double-scaled scores).
