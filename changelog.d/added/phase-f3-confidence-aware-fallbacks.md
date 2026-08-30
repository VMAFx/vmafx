### Added — `vmaf-tune auto` Phase F.3 confidence-aware fallbacks (ADR-0325)

`vmaf-tune auto` now consults the conformal-VQA prediction interval
(ADR-0279 / `Predictor.predict_vmaf_with_uncertainty`) at every
`(rung, codec)` cell to decide whether the F.2 GOSPEL/FALL_BACK gate
should be overridden. Three outcomes (`SKIP_ESCALATION`,
`RECOMMEND_ESCALATION`, `FORCE_ESCALATION`) are recorded per cell in
`plan.metadata.confidence_aware_escalations[]`. The two width gates
(`tight_interval_max_width`, `wide_interval_min_width`) come from a
calibration sidecar shipped with the conformal-VQA pipeline; when no
sidecar is found the loader falls back to the documented Research-0067
defaults (2.0 / 5.0 VMAF) and emits a one-line warning. Per-corpus
sidecars override the defaults transparently. New helper
`_confidence_aware_escalation(verdict, interval, thresholds)` is pure
and unit-tested in isolation.
