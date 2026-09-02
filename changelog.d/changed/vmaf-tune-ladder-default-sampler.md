- **`vmaf-tune` Phase E ladder default sampler is now wired
  ([ADR-0307](docs/adr/0307-vmaf-tune-ladder-default-sampler.md)).**
  `ladder.build_ladder()` / `ladder.build_and_emit()` no longer raise
  `NotImplementedError` when called with `sampler=None`. The default
  sampler composes `corpus.iter_rows` (Phase A encode + score) with
  `recommend.pick_target_vmaf` (smallest-CRF-meeting-target predicate)
  over the canonical 5-point CRF sweep
  `DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` at the codec
  adapter's mid-range preset (`"medium"` for libx264 / libx265 /
  libsvtav1). The `SamplerFn` seam stays open: callers needing a
  finer grid or a non-CRF-based search pass an explicit `sampler=`.
  Companion research digest:
  [Research-0079](docs/research/0079-vmaf-tune-ladder-default-sampler.md).
