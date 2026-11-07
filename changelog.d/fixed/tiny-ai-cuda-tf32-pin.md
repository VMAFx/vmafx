- **The tiny-AI parity gate pins strict fp32 on the CUDA EP and names the `transnet_v2` provider gap** (#2947).
  `scripts/ci/tiny_ai_cross_device_parity_gate.py` creates every `CUDAExecutionProvider` session with the
  provider option `use_tf32=0`: onnxruntime-gpu 1.31.1 leaves TF32 enabled by default, which pushed 13/24 fp32
  tiny models past the documented 1e-4 bound (worst 3.9e-02; with the pin, worst 3.8e-05), and the
  session-config entry `ep.cuda.use_tf32` is silently ignored by that release. `transnet_v2` runs as a bind
  probe: its CUDA leg cannot open on 1.31.1 (`element_type 6 (int32) not implemented for ScatterND operator`)
  and is recorded as an expected provider gap instead of failing the gate, while any other `transnet_v2`
  failure still fails. The JSON and Markdown summaries name the TF32 mode and the per-model provider gaps.
