# Changelog fragment

- `float_ssim_cuda`: wire `enable_db` and `clip_db` options (previously silently
  dropped). Passing `--feature float_ssim_cuda:enable_db=true` now converts the
  SSIM score to dB domain (-10·log₁₀(1−SSIM)), matching the CPU `float_ssim`
  extractor. `clip_db=true` caps the dB value to the finite ceiling computed
  from bit-depth and frame dimensions (same formula as `float_ssim.c:init`).
  Source: non-CUDA GPU bug audit 2026-05-16, copy-paste parity audit 2026-05-16.
