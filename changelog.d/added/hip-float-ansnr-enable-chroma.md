- **`float_ansnr_hip` `enable_chroma` option** (ADR-0453 parity):
  `FloatAnsnrStateHip` gains `bool enable_chroma` (default `false`) and
  `unsigned n_planes`. When `enable_chroma=true`, the extractor dispatches
  the ANSNR kernel once per plane (Y, Cb, Cr) using per-plane geometry and
  separate readback slots, then emits `float_ansnr_cb`, `float_anpsnr_cb`,
  `float_ansnr_cr`, and `float_anpsnr_cr` in addition to the luma scores.
  YUV400P sources clamp to luma-only regardless of the flag. Mirrors CPU
  PR #947 and CUDA PR #957. Chroma scores use the same double-precision
  host accumulation as luma.
