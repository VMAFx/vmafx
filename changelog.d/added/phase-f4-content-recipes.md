### Added — `vmaf-tune auto` Phase F.4 per-content-type recipe overrides (ADR-0325)

`vmaf-tune auto` now applies per-content-type **recipe overrides**
before the F.2 short-circuits evaluate. The four named recipes ship
in `tools/vmaf-tune/src/vmaftune/auto.py::_CONTENT_RECIPE_TABLE`:

- `animation` — single-rung ladder + aggressive saliency; predictor
  target VMAF lifted +2 (animation compresses uniformly on flat
  colour fields).
- `screen_content` — `very_aggressive` saliency intensity (high QP
  on background, near-lossless on text regions); predictor target
  +1.
- `live_action_hdr` — narrowed F.3 conformal-tight gate (1.2 vs the
  SDR default 2.0); any wide predictor interval on HDR is suspect
  given the SDR-trained predictor (ADR-0279).
- `ugc` — widened F.3 tight gate (3.0); UGC's higher upstream-encode
  noise makes wider predictor intervals the baseline. Predictor
  target nudged -1 because UGC's perceptual ceiling is capped by
  source-side artefacts.

The recipe class lands in `plan.metadata.recipe_applied`
(`{animation, screen_content, live_action_hdr, ugc, default}`) and
the override dict in `plan.metadata.recipe_overrides`. Per memory
`feedback_no_test_weakening`: `target_vmaf_offset` shifts only the
predictor's `effective_predictor_target_vmaf`; the input
`--target-vmaf` (the gate that ships models) is preserved verbatim.

Every threshold value shipped at F.4 is `[provisional, calibrate
against real corpus in F.5]`. F.5 closes the calibration loop once
F.4 has emitted enough labelled recipe applications to fit the
placeholders empirically.

37 new assertions in `tools/vmaf-tune/tests/test_auto_recipe_overrides.py`
cover recipe lookup, the read-only-factory invariant, per-class
trigger semantics, threshold narrowing without violating the
constructor invariant, JSON metadata recording, and the ordering
guarantee (recipe fires before F.2). Docs at
[`docs/usage/vmaf-tune.md` § Per-content-type recipes
(F.4)](docs/usage/vmaf-tune.md#per-content-type-recipes-f4). See
[ADR-0325](docs/adr/0325-vmaf-tune-phase-f-auto.md) §F.4 status
update.
