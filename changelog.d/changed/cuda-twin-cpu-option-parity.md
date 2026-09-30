- **CUDA twins take the CPU extractor's options and arithmetic (ADR-1373).** `psnr_cuda`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the same `core/src/feature/psnr_score.h` helpers as the CPU
  extractor, `apsnr_*` aggregates included; `integer_ssim_cuda` accepts
  `enable_db` / `clip_db`; `float_ssim_cuda` accepts `enable_lcs` (a device
  kernel reduces the L, C and S terms) / `enable_db` / `clip_db`; and
  `float_motion_cuda` accepts `motion_max_val` and weights its debug `motion`
  score by `motion_fps_weight` like the CPU. Before, a model or a
  `--backend cuda --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. `float_ssim_cuda` now computes the
  CPU's per-pixel `l * c * s` with the CPU's rounding and rounds the frame
  mean to fp32, so with `enable_db` identical frames report the CPU's value
  (identical flat frames: 72.247 dB, where the twin reported `+inf`);
  `integer_ssim_cuda` computes each pixel's term as the CPU does;
  `motion_v2_cuda` publishes the CPU's fps-weighted, capped SAD score and
  emits `motion2_v2` / `motion3_v2` for a one-frame input; `psnr_cuda` sums
  every frame into `apsnr_*` under `--subsample`, and its chroma accumulators
  can no longer be cleared while the chroma kernels run. `float_ssim_cuda`
  still accepts `enable_chroma`, which the CPU `float_ssim` does not have, and
  warns that it is ignored. Measured on an RTX 4090: the PSNR, `motion_v2`
  and `float_motion` options give the CPU's scores exactly, `ssim` stays
  within 7.3e-13 dB and `float_ssim` within 6.9e-6 dB of the CPU; see
  [the CUDA backend guide](docs/backends/cuda/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins).
