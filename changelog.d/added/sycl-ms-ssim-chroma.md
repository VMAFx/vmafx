- **`enable_chroma` now computes chroma on the SYCL MS-SSIM twin instead of being
  accepted and ignored.** The option derived `n_planes` and nothing ever read it,
  so setting it changed nothing on the GPU and the chroma features a model saw
  came from the CPU twin under the ADR-0530 name fallback. Each plane now has its
  own geometry, staging buffer and pyramid, and `float_ms_ssim_cb` / `_cr` are
  advertised by the GPU twin. Verified on an Intel Arc A380 against the CPU twin
  with a textured-chroma 4:2:0 fixture: identical to all six emitted digits across
  three frames (ADR-1299).
