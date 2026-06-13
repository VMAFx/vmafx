- **fix(core):** Restore `vmaf_fex_integer_motion_v2` CPU registration in
  `feature_extractor.cpp` — the compiled translation unit. PR #673 (ADR-1052)
  correctly updated `feature_extractor.c` but `meson.build` compiles
  `feature_extractor.cpp`; the `.cpp` file still carried the stale
  "removed" comment and omitted `extern vmaf_fex_integer_motion_v2` and
  `&vmaf_fex_integer_motion_v2` from `feature_extractor_list[]`. Result:
  `vmaf_get_feature_extractor_by_name("motion_v2")` returned `NULL` on
  every CPU-only static build, failing `test_integer_motion_v2_coverage`.
