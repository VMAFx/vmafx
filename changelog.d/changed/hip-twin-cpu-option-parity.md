- **Four HIP twins take the CPU extractor's options (ADR-1382).** `psnr_hip`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the CPU's own `core/src/feature/psnr_score.h`, `apsnr_*` aggregates
  included; `integer_ssim_hip` accepts `enable_db` / `clip_db`,
  `float_ssim_hip` `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s`
  computed on the device), and `float_motion_hip` `motion_max_val`, with its
  debug `motion` score now weighted by `motion_fps_weight` like the CPU's.
  Before, a model that set one of these options computed the feature on the
  CPU, and naming the twin with the option failed with `unknown option`. Both
  SSIM twins score an identical window exactly 1, so with `enable_db`
  identical frames report the CPU's `+inf` or `clip_db` ceiling. Not yet
  measured on AMD hardware; see
  [the HIP backend guide](docs/backends/hip/overview.md#rc3-cpu-parity-motion-tiny-frames-and-cpu-options-2026-09-30).
