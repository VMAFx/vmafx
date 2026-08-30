### Added — `vmaf-tune recommend` and `ladder` consume conformal intervals

The conformal-VQA prediction surface shipped in PR #488 (ADR-0279) now
drives two more downstream consumers in `tools/vmaf-tune`:

* **`recommend`** — the per-clip CRF target search (`vmaf-tune
  recommend --with-uncertainty`) reads the per-row conformal
  interval and short-circuits the search the moment a row whose
  lower bound clears the target is observed. When the predictor is
  uncertain (interval width above the wide-band gate, default 5.0
  VMAF per Research-0067), the search refuses to short-circuit and
  the result is tagged `(UNCERTAIN)`. Library API:
  `vmaftune.recommend.pick_target_vmaf_with_uncertainty`.

* **`ladder`** — the ABR ladder builder gains
  `prune_redundant_rungs_by_uncertainty` (drops adjacent rungs
  whose conformal intervals overlap above the default 0.5 fraction)
  and `insert_extra_rungs_in_high_uncertainty_regions` (inserts a
  synthetic mid-rung in any wide-interval gap so the operator pays
  one extra encode where ladder choices have the most empirical
  impact). Library API: `vmaftune.ladder.apply_uncertainty_recipe`.

Both recipes share the same `ConfidenceThresholds` dataclass and
sidecar loader as the auto-driver F.3 work (PR #495), so a single
calibration JSON drives `auto`, `recommend`, and `ladder` without
divergence. Thresholds default to the documented Research-0067 floor
(tight=2.0, wide=5.0 VMAF) when no sidecar is shipped.

Per the `feedback_no_test_weakening` rule in `CLAUDE.md`, this
change only affects search cost / ladder-rung selection. The
production-flip gate that decides which encodes get shipped lives in
`predictor_validate.py` and stays untouched.

Docs: `docs/usage/vmaf-tune-recommend.md`,
`docs/usage/vmaf-tune-ladder.md`. ADR status update:
`docs/adr/0279-fr-regressor-v2-probabilistic.md` "Status update
2026-05-09".
