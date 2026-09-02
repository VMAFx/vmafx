- **ai/scripts coverage round 3**: added `test_calibrate_phase_f_recipes_unit.py`
  (50 tests exercising the 7 pure helper functions — `mos_to_vmaf_proxy`,
  `saliency_benefit_to_intensity`, `_iter_corpus_rows`, `_ugc_target_vmaf_offset`,
  `_ugc_tight_interval_width`, `_resolution_dominance`,
  `_ugc_saliency_benefit_fraction`, and `calibrate()` — that the existing
  provenance-only smoke test left uncovered) and
  `test_analyze_knob_sweep_unit.py` (29 tests covering `_stable_knob_repr`,
  `_slug`, `_closest_bare_at_bitrate`, `write_slice_csv`, and
  `write_summary_md`).  All 79 new tests run without GPU, corpus, or model
  downloads.
