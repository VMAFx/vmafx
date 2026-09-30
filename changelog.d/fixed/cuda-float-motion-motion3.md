- **`float_motion_cuda` emits `motion3`, like the CPU `float_motion`.** The
  CUDA twin wrote `motion` and `motion2` only, so `--backend cuda --feature
  float_motion` lost `VMAF_feature_motion3_score` without a warning. It now
  publishes the CPU's `motion3` (the fps-weighted `motion2`, blended by
  `motion_blend_factor` / `motion_blend_offset` and capped at
  `motion_max_val`; frame 0 from the first SAD, `0` for a one-frame input)
  and accepts both blend options (aliases `mbf` / `mbo`). On an RTX 4090 it
  stays within 2.8e-6 of the CPU on the Netflix pair, like `motion2`. The
  SYCL, HIP and Metal twins still write no `motion3`
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).
