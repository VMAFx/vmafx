- **HIP extractors share one upload of each frame (ADR-1408).** Every HIP
  extractor used to copy the planes it reads to the device itself and wait
  for that copy, so a run with several extractors uploaded the same frame
  several times: 31 planes for a 4:2:0 frame pair with thirteen extractors.
  The `VmafContext` now uploads each plane once per frame and the extractors
  read that copy (`psnr`, `float_psnr`, `float_moment`, `ciede`, `ssim`,
  `float_ssim`, `vif`, `float_vif`, `adm`, `float_adm`, `motion`, `motion_v2`
  and `float_motion` on HIP). No score changes: every metric of every frame is
  bit-identical before and after. On a gfx1036
  `--backend hip --model version=vmaf_float_v0.6.1` goes from 57.3 to 46.9 ms
  per 1920x1080 frame (17.5 to 21.3 frames per second) and from 294 to 226 ms
  per 3840x2160 frame; `vmaf_v0.6.1` and runs whose time is all device
  kernels are unchanged. `--subsample` stays correct: a plane an extractor
  still reads is not overwritten.
