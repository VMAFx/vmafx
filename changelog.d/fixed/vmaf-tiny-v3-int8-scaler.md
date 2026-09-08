**Declare `onnx_has_scaler` in `vmaf_tiny_v3.int8.json`** (ADR-0174, ADR-0275)

- `model/tiny/vmaf_tiny_v3.int8.json` now declares `"onnx_has_scaler": true`.
  `vmaf_tiny_v3.int8.onnx` bakes the StandardScaler into the graph as `Sub` /
  `Div` Constant nodes; without the declaration `core/src/libvmaf.c` normalised
  the canonical-6 feature vector a second time and the tiny-model score was
  garbage. Measured on the Netflix `src01_hrc00/hrc01_576x324` pair (48 frames,
  CPU backend): pooled `vmaf_tiny_model` mean **16.020865 → 71.952113**, against
  an fp32 baseline of **72.359458**; per-frame PLCC vs fp32 **0.975443 →
  0.999876** (drop 0.000124, inside the sidecar's declared 0.01 budget).
- Added a registry consistency gate — every `model/tiny/*.int8.onnx` whose graph
  contains both `Sub` and `Div` must have a companion sidecar declaring
  `"onnx_has_scaler": true`. Enforced in `core/test/dnn/test_registry.sh`,
  `python/test/model_registry_schema_test.py`, and
  `ai/scripts/validate_model_registry.py`.
