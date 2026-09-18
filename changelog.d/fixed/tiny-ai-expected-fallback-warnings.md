- **Tiny-AI models no longer print a WARNING for fallbacks that are expected
  and handled.** On an ONNX Runtime build without a kernel for a quantised op,
  every run of an int8-redirected model such as `nr_metric_v1` printed
  `libvmaf WARNING libvmaf dnn CreateSession: Could not find an implementation
  for ConvInteger(10)` before the loader fell back to the fp32 baseline and
  scored the clip correctly. The same happened when an execution provider
  registered but its hardware was absent (a CUDA-enabled ONNX Runtime on a
  machine without an NVIDIA GPU) and the session fell back to the CPU. Both
  are now logged at DEBUG, as ADR-1032 and the ADR-0113 fallback always
  specified; a session that cannot be created on any path still logs a
  WARNING. Scores are unchanged.
