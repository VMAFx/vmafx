- **Metal feature extractors unwind without `goto`.** Seven Metal host
  wrappers (`float_adm`, `float_moment`, `float_ms_ssim`, `float_psnr`,
  `integer_ciede`, `integer_psnr_hvs`, `ssimulacra2`) release a failed init
  through one null-safe helper instead of a chain of cleanup labels, and the
  long `ssimulacra2` colour-conversion and submit functions are split under
  60 lines. Scores and the C API do not change.
