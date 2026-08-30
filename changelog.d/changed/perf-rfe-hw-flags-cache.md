# Cache rfe_hw_flags bitmask across frames (F2-B)

`vmaf_read_pictures` no longer re-scans the registered feature extractor list to
compute the CUDA host/device flag bitmask on every frame. The bitmask is now computed
once on the first frame (or after `vmaf_use_feature` adds a new extractor) and cached
in `VmafContext.rfe_hw_flags_cache`. Subsequent frames use the cached value via a
single boolean guard (`rfe_hw_flags_dirty`).

**Motivation:** The per-frame O(n_extractors) scan was identified as a hot-cache
pressure point in perf-audit-pipeline-2026-05-16 (finding F2-B). For a typical 5-extractor
run the saving is small (5 comparisons/frame eliminated), but it removes unnecessary
cache-line pressure on `registered_feature_extractors.fex_ctx` on every frame.

**No behavior change:** The extractor set is always fixed before the frame loop;
`vmaf_use_feature` invalidates the cache so any late registration is handled correctly.

**Related:** perf-audit-pipeline-2026-05-16 (F2-B).
