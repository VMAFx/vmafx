- **`float_ssim_sycl` with `enable_db` no longer reports tens of dB below the
  CPU on near-identical frames.** The CPU rounds each frame's SSIM mean to fp32
  before converting it to dB, so a frame within half an fp32 step of 1 scores
  exactly 1 and reports `+inf` or the `clip_db` ceiling; the twin kept the
  double mean and reported a finite value (93.6 dB against the CPU's 121 dB on
  the first frames of a 4K pair). The twin now rounds the `float_ssim` and
  `float_ssim_l/c/s` means the same way; linear scores move by less than 6e-8.
