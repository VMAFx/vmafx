- **Metal `integer_ssim` kernel.** The Metal backend now implements the
  `integer_ssim` extractor (feature `ssim`) via `integer_ssim_metal.mm` +
  `integer_ssim.metal`, completing the cross-backend integer-SSIM set
  (ADR-0564) alongside the existing CUDA / HIP / SYCL kernels. It mirrors the
  `float_ssim_metal` two-pass separable-Gaussian scaffold with the fixed-point
  arithmetic of the CPU reference. `--backend metal --feature ssim` now scores
  on Apple Silicon instead of returning no kernel.
