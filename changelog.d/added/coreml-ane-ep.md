- **Apple CoreML execution provider for tiny-AI inference
  ([ADR-0365](../docs/adr/0365-coreml-ep-wiring.md)).** Four new
  `--tiny-device` selectors (`coreml`, `coreml-ane`, `coreml-gpu`,
  `coreml-cpu`) and matching `VmafDnnDevice` enum values 5..8
  (append-only) wire the `CoreMLExecutionProvider` into the fork's
  ORT dispatch layer. The `coreml-ane` selector pins
  `MLComputeUnits=CPUAndNeuralEngine` for highest perf-per-watt on
  M-series Apple silicon (M1/M2/M3/M4); the unscoped `coreml` lets
  the EP auto-route across ANE / Metal-GPU / CPU. The Linux build
  degrades cleanly (EP absent → graceful CPU-EP fallback in
  `vmaf_ort_open()`); macOS hosts hit the real ANE / Metal path.
  Apple-side parallel to ADR-0332's OpenVINO NPU wiring. End-to-end
  ANE silicon validation deferred until Apple-silicon hardware
  access; the Linux CI lane covers the open-and-fallback path on
  every push.
