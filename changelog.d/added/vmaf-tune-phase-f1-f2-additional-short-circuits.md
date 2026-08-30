### `vmaf-tune auto` — three additional F.1/F.2 short-circuit predicates

Three new short-circuit predicates (#8, #9, #10) are appended to
`SHORT_CIRCUIT_PREDICATES` in `tools/vmaf-tune/src/vmaftune/auto.py`:

* **`low-complexity`** — skips `recommend.coarse_to_fine` when the
  codec adapter's probe-encode bitrate (`meta.complexity_score`) is
  below 200 kbps. Content that compresses trivially (slides, test cards)
  does not need a CRF sweep.
* **`baseline-meets-target`** — skips the full predictor sweep when the
  default-CRF encode already meets the target VMAF (`meta.baseline_vmaf
  >= plan_state.target_vmaf`). No sweep needed when the baseline is
  sufficient.
* **`no-two-pass`** — skips the two-pass calibration stage when the
  codec adapter declares `supports_two_pass = False` (ADR-0333). All
  hardware encoders and most software encoders except `libx265` fire
  this.

`SourceMeta` gains `complexity_score` and `baseline_vmaf` fields (both
default `0.0`); `PlanState` gains `adapter_supports_two_pass` (default
`None`). Smoke mode degrades gracefully — new predicates do not fire when
fields are at zero/None defaults. 28 new unit tests in
`tests/test_auto_phase_f1_f2.py`.
