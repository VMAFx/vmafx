- **fix(core):** Re-register CPU `motion_v2` feature extractor in `meson.build` and
  `feature_extractor_list[]`; `vmaf_get_feature_extractor_by_name("motion_v2")` now returns
  a valid extractor on CPU-only builds (ARM64 and x86-64). Fixes `test_motion_v2_missing`
  and related ARM64 CI failures introduced by PR #532. Fix `test_motion_three_frame` test
  ordering: `get_score(motion2_score)` must be called after `flush()` because the
  pipelined-rename port defers motion2 emission to flush time. ([ADR-1052](../docs/adr/1052-arm-motion-v2-re-register.md))
