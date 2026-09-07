- `motion_fps_weight` is now applied exactly once on the CUDA, SYCL and
  HIP `motion` twins. Their host-side `motion3` post-process re-applied
  the weight to an input every caller had already weighted and clipped,
  so `VMAF_integer_feature_motion3_score` carried `motion_fps_weight`
  **squared** whenever the option was set away from its `1.0` default
  (`motion2_score` was unaffected, and at the default weight the
  squaring is invisible — which is why every existing parity test
  passed). Measured on an RTX 4090 at `motion_fps_weight = 0.6`:
  `cpu = 14.48987751` vs `cuda = 8.69392654`, a `5.8` absolute drift
  against a `1e-4` gate. The three `test_<backend>_motion3_parity`
  tests now pin a non-default weight and assert CPU/GPU parity on the
  derived `integer_motion3_mfw_0.6` key. See ADR-1216.
