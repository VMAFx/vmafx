- `feature_extractor_list[]` duplicate-registration bug (ADR-0544):
  the Vulkan block held 67 entries instead of 18 (two extractors
  registered 11x each, seven others 6x each); the SYCL block held 17
  instead of 11 (six extractors registered 2x). The first-match
  `vmaf_get_feature_extractor_by_name()` masked the bug, but the
  ctx-pool's iterator path allocated one pool entry per duplicate and
  ran `init`/`extract`/`flush` 2x-11x per picture on the affected GPU
  twins. The deduped list shaves the affected backends down to one
  entry per extractor; a new `vmaf_feature_extractor_list_audit()`
  fires from `vmaf_init()` so any future regression fails fast.
