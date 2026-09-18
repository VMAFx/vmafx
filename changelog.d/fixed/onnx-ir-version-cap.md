- **The AI tooling no longer pulls in an `onnx` whose models the pinned
  runtime cannot load.** `onnx` 1.23 writes models at IR version 14, and
  onnxruntime 1.30, the runtime this repository pins, reads at most IR 13.
  The `ai` and ensemble-training-kit packages now require `onnx<1.23` until
  onnxruntime catches up.
