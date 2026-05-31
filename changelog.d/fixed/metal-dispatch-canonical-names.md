- **Metal dispatch table — canonical score names + missing
  `float_motion` entries.** `core/src/metal/dispatch_strategy.c`'s
  `g_metal_features[]` carried three short-form aliases
  (`motion2_v2_score`, `motion2_score`, `motion3_score`) that never
  matched the canonical names each Metal motion extractor publishes
  in its own `provided_features[]` array. As a result
  `vmaf_metal_dispatch_supports()` returned `0` for the canonical
  query string and every routing call silently fell back to CPU
  for `integer_motion_metal` (motion2) and `integer_motion_v2_metal`
  (motion2-v2). Additionally `float_motion_metal`'s two score-level
  names (`VMAF_feature_motion_score`, `VMAF_feature_motion2_score`)
  were completely absent — the extractor-level
  `"float_motion_metal"` / `"float_motion"` keys alone were
  insufficient. Fix: replace the short-form aliases with the
  canonical names from each extractor's `provided_features[]`,
  add the missing `float_motion_metal` score-level entries, and
  drop the dead `motion3_score` (not implemented on Metal per
  `integer_motion_metal.mm:15`). Identified by metal-reviewer
  agent audit 2026-05-30 (HIGH severity).
