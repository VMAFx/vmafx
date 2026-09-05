- Add `cambi_high_res_speedup` (alias `hrs`) to the Metal CAMBI twin option table.
  The default model (`vmaf_v1.0.16_3d0h`) configures `cambi_high_res_speedup: 1080`,
  `cambi_vis_lum_threshold: 0.06`, and `cambi_max_val: 17`. The Metal twin
  (`integer_cambi_metal`) previously lacked `cambi_high_res_speedup` in its option table,
  causing model dispatch to fall back to the CPU extractor and feature names to diverge.
  `integer_cambi_metal.mm` now carries `cambi_high_res_speedup` in its option table,
  applies high-resolution speedup window adjustment and post-spatial-mask decimation
  for resolutions >= 1080p in parity with `cambi.c`, and emits feature scores matching
  the CPU extractor under the option-qualified feature key. `submit_fex_metal` also
  restores its three scratch-buffer handles on every exit path, so the per-scale pointer
  rotation no longer leaks into the next frame.
