- **Encoder knob-space Pareto-frontier analysis scaffold (ADR-0305 /
  Research-0077, companion to Research-0063).** Ships the methodology
  + scripts for the 12,636-cell knob sweep (9 sources × 6 codec
  families × 3 rate-control modes × ~78 knob combinations per codec)
  that drives `tools/vmaf-tune/codec_adapters/*` recipe defaults.
  Pareto frontiers are stratified **per `(source, codec, rc_mode)`
  slice** rather than as a single global hull — Research-0063 showed
  that a global hull collapses the rate-control flip and produces
  consensus recipes that regress NVENC h264/hevc by ~4 VMAF at
  cq=30 against the bare encoder defaults. Adds
  `ai/scripts/analyze_knob_sweep.py` (computes per-slice Pareto
  hulls on `(bitrate_kbps, vmaf_score)` with `encode_time_ms` as
  tiebreaker; emits per-slice CSVs + a markdown summary; carries
  the regression-detection check that gates ship-candidate
  recipes) and `ai/tests/test_knob_sweep_analysis.py` (synthetic
  20-row JSONL fixture; covers `test_pareto_frontier_smoke`,
  `test_stratification_keys`, `test_recipe_regression_detection`).
  The actual `comprehensive.jsonl` sweep file lives under
  `runs/phase_a/full_grid/` (gitignored) and is generated locally;
  headline findings on the populated Pareto frontiers land via a
  follow-up commit when the sweep completes (~3h ETA from this
  PR). Reproducer:
  `pytest ai/tests/test_knob_sweep_analysis.py -v`.
