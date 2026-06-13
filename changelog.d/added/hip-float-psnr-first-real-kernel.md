- **HIP `float_psnr_hip` — first real AMD GPU kernel
  ([ADR-0254](../docs/adr/0254-hip-second-consumer-float-psnr.md)).**
  Promotes `float_psnr_hip` from a scaffold stub (returning `-ENOSYS`)
  to a functional HIP kernel. Device code in
  `core/src/feature/hip/float_psnr/float_psnr_score.hip` implements
  the same per-pixel `(ref - dis)^2` warp reduction as the CUDA twin;
  the host-side `10 * log10(peak^2 / noise)` formula is identical.
  When `enable_hipcc=true`, `hipcc` compiles the `.hip` source to a
  HSACO fat binary which is embedded via `xxd -i` and loaded at runtime
  via `hipModuleLoadData` + `hipModuleLaunchKernel` — the direct HIP
  module-API analog of CUDA's `cuModuleLoadData` + `cuLaunchKernel`.
  Without `enable_hipcc` the extractor registers normally but `init()`
  returns `-ENOSYS`, preserving the scaffold contract for non-ROCm
  builds. This proves the HIP runtime pattern (kernel compilation,
  embedding, module loading, async dispatch, event-gated readback)
  that the remaining 10 HIP stubs will mechanically follow.
