- **GPU float-SSIM auto-scale fallback (ADR-1324):** automatic model dispatch
  now checks the first picture's dimensions before initializing CUDA, SYCL,
  HIP or Metal `float_ssim`. When `scale=0` resolves above the GPU twins'
  current scale-1 capability, only that feature is computed by CPU
  `float_ssim` with the same options; small frames and explicit `scale=1` stay
  on device. Explicitly named GPU extractors and malformed option values retain
  their existing errors.
