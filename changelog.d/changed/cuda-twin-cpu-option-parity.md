- **Four CUDA twins take the CPU extractor's options (ADR-1373).** `psnr_cuda`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the same `core/src/feature/psnr_score.h` helpers as the CPU
  extractor, `apsnr_*` aggregates included; `integer_ssim_cuda` accepts
  `enable_db` / `clip_db`; `float_ssim_cuda` accepts `enable_lcs` (a device
  kernel reduces the L, C and S terms) / `enable_db` / `clip_db`; and
  `float_motion_cuda` accepts `motion_max_val` and weights its debug `motion`
  score by `motion_fps_weight` like the CPU. Before, a model or a
  `--backend cuda --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. `float_ssim_cuda` scores an identical
  window exactly 1, so with `enable_db` identical frames report the CPU's
  `+inf` or `clip_db` ceiling. `float_ssim_cuda` no longer declares
  `enable_chroma`, which the CPU `float_ssim` does not have and the twin never
  implemented. Not yet measured on an NVIDIA GPU; see
  [the CUDA backend guide](docs/backends/cuda/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins).
