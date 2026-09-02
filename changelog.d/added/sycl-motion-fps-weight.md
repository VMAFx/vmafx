### SYCL `motion_v2` gains `motion_fps_weight` option

`integer_motion_v2_sycl.cpp` now exposes the `motion_fps_weight` (alias
`mfw`) feature parameter, matching the existing option in `motion_sycl`,
`motion_cuda`, and the CPU `integer_motion.c` reference. The weight is
applied to the `motion2_v2_score` in `flush_fex_sycl` before the score
is appended to the feature collector. Default is `1.0` (no-op). Range:
`[0.0, 5.0]`. Flags: `VMAF_OPT_FLAG_FEATURE_PARAM`.

Previously, passing `motion_fps_weight` via `vmaf_use_features_with_opts`
to `motion_v2_sycl` silently fell through the options table and used the
default, producing silently-wrong scores when the caller expected a
non-default weight.
