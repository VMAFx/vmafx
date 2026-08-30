- **`motion_v2` public option surface — duplicate registration of motion v1's
  knobs.** Per [ADR-0337](../docs/adr/0337-motion-v2-public-api-options.md),
  `motion_v2` (the pipelined integer motion extractor) now accepts the same
  option set that motion v1 already exposes via [ADR-0158](../docs/adr/0158-netflix-1486-motion-updates-verified-present.md):
  `motion_force_zero` (alias `force_0`), `motion_blend_factor` (`mbf`),
  `motion_blend_offset` (`mbo`), `motion_fps_weight` (`mfw`),
  `motion_max_val` (`mmxv`), `motion_five_frame_window` (`mffw`), and
  `motion_moving_average` (`mma`). Adds the `VMAF_integer_feature_motion3_v2_score`
  feature to `motion_v2`'s `provided_features[]` (closes the motion3
  coverage gap on the v2 extractor). Mirrors the upstream four-commit
  cluster `856d3835` / `c17dd898` / `a2b59b77` / `4e469601` that lands
  these on motion_v2; the fork ships the option surface and 3-frame
  motion3 emission. `motion_five_frame_window=true` returns `-ENOTSUP`
  at `init()` and points at the ADR — the picture-pool plumbing
  required by the 5-frame mode (`prev_prev_ref` field on
  `VmafFeatureExtractor` + `n_threads * 2 + 2` picture-pool sizing in
  `vmaf_read_pictures`) is deferred to a follow-up PR per ADR-0337
  §Decision and mirrors [ADR-0219](../docs/adr/0219-motion3-gpu-coverage.md)
  §Decision's GPU motion3 precedent. Resolves the architectural
  question deferred by PRs #453 and #460. Netflix golden gate
  (ADR-0024) untouched (motion v1 unchanged); motion v2 default
  option values are arithmetic identities for the existing
  `testdata/scores_cpu_motion_v2_*.json` snapshot range.
