- **`float_motion_sycl` honours `motion_force_zero` and weights its debug
  score.** The twin declared `motion_force_zero` but ignored it and emitted
  real motion scores; it now emits zeros, like the CPU `float_motion`. Its
  debug `VMAF_feature_motion_score` now carries `motion_fps_weight`, as on the
  CPU, instead of the unweighted SAD. The SSIM page no longer documents an
  `enable_chroma` option and `_cb` / `_cr` outputs that the `ssim` extractor
  does not have; it lists `enable_db` and `clip_db`
  ([SSIM](docs/metrics/ssim.md#options)).
