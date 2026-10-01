- **Every CUDA kernel is built without FMA contraction, and
  `float_ms_ssim_cuda` is bit-identical to the CPU.** nvcc fuses `a * b + c`
  into one FMA by default; six of the 21 CUDA kernels were built with
  `--fmad=false` and fifteen were not, while the CPU build and the SYCL twins
  never fuse. All kernels now take one flag list
  (`cuda_device_strict_fp_args`), so a plain multiply-add rounds twice on the
  device as it does on the host, and a kernel whose CPU reference fuses on
  purpose writes the fused operation explicitly
  ([ADR-1403](docs/adr/1403-cuda-strict-fp-every-kernel.md)). Measured on an
  RTX 4090 against `--backend cpu` at `--precision max`: `adm`, `vif`,
  `motion`, `motion_v2`, `psnr`, `psnr_hvs`, `float_psnr`, `float_moment`,
  `cambi`, `float_adm`, `float_ssim`, `ssim`, `ssimulacra2`, `speed_chroma` and
  `speed_temporal` produce exactly the values they produced before. `float_ms_ssim_cuda`, whose
  kernels now follow the CPU extractor operation for operation, goes from up
  to 4.4e-6 away to bit-identical on every frame of the Netflix pair, the
  1080p checkerboard pairs and BBB 3840x2160, per-scale `enable_lcs` outputs
  included. `ciede_cuda`, `float_vif_cuda` and `float_motion_cuda` move in
  their last digits and stay inside the cross-backend tolerance where they
  were. No twin is measurably slower at 3840x2160. Re-run any stored CUDA
  output of those four twins.
  `-Denable_nvcc=false` (CUDA kernels through clang) configures again and
  agrees with the nvcc build on 18 of 19 twins
  ([CUDA backend](docs/backends/cuda/overview.md#floating-point-model-no-fma-contraction-adr-1403)).
