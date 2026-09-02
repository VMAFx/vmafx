- `float_ms_ssim` HIP backend now honours `enable_db` and `clip_db` options
  (previously silently ignored, causing raw linear scores to be returned even
  when dB output was requested). SYCL backend gains all three missing options:
  `enable_lcs`, `enable_db`, and `clip_db`. CUDA latent bug fixed: the backend
  computed a dB-converted score but appended the unconverted linear value;
  corrected to append `score` (no change at default `enable_db=false` settings).
  ADR-1078.
