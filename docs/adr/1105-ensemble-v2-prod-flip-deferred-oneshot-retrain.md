<!-- markdownlint-disable MD013 MD060 -->
# ADR-1105: `fr_regressor_v2_ensemble` production flip deferred to the one-shot post-RC retrain

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: Lusoris
- **Tags**: `ai`, `models`, `rc`, `docs`

## Context

[ADR-0321](0321-fr-regressor-v2-ensemble-full-prod-flip.md) flipped the
five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
`model/tiny/registry.json` from smoke to production: it shipped real
LOSO-validated ONNX weights (gate verdict `PROMOTE`, mean PLCC ≈ 0.997),
per-seed sidecars, and `smoke: false`. Those weights were trained with a
codec one-hot of width 14 (`codec_vocab` = 12 codec entries + 2 norm
dims).

`codec_vocab` was subsequently trimmed to 6 (`x264`, `x265`,
`libsvtav1`, `libvvenc`, `libvpx-vp9`, `unknown`; see
`model/tiny/fr_regressor_v2_ensemble_v1.json`). This made the
production ONNX input dimension stale: a model expecting `[batch, 14]`
can no longer be fed the current `[batch, 6]` codec one-hot, which
surfaced as an `eval_probabilistic_proxy.py --smoke` load failure.

PR #865 fixed the load path by regenerating the five ONNX files at the
correct `[batch, 6]` width, but it did so with the trainer's `--smoke`
mode — one epoch on a synthetic corpus, i.e. throwaway placeholder
weights, not a production fit. The registry was correspondingly set to
`smoke: true` with new sha256 values. Two side effects of that PR were
not intended:

1. The `license`, `license_url`, and `sigstore_bundle` fields were
   dropped from the five rows. This is a pure regression — every
   registry entry (smoke or not) must carry license metadata, and the
   `test_every_entry_has_license_metadata` invariant enforces it. The
   fields are restored unconditionally in this PR.
2. The five rows now claim neither production nor a consistent provenance
   record: the on-disk ONNX are smoke (new sha), while the retained
   per-seed sidecars still describe the older `[batch, 14]` production
   weights (old sha, `PROMOTE`). The
   `test_fr_regressor_v2_ensemble_seed_rows_are_production` invariant
   (added by ADR-0321) consequently fails on `smoke is False`.

Producing real production weights at `codec_vocab = 6` requires
re-running `export_ensemble_v2_seeds.py` against the corpus — a full
retrain/re-export. The operator has locked all model retraining into a
single one-shot step to be run only after the toolchain reaches RC and
the feature numbers are frozen, explicitly to avoid retraining models
repeatedly. The ensemble is in scope for that one-shot retrain. Doing a
piecemeal ensemble-only retrain now would contradict that decision and
risk shifting numbers that the one-shot run is meant to freeze.

## Decision

For the release candidate, ship the ensemble seed rows honestly as smoke
placeholders and defer the production flip to the locked one-shot retrain:

1. Restore `license`, `license_url`, and `sigstore_bundle` on all five
   rows (the #865 regression). Keep `smoke: true` and the regenerated
   `[batch, 6]` sha256 values, which match the ONNX actually shipped.
   Update each row's `notes` to state plainly that these are smoke
   placeholders pending the one-shot production re-export.
2. Keep the `test_fr_regressor_v2_ensemble_seed_rows_are_production`
   assertions verbatim (they remain the target production contract), but
   mark the test `@pytest.mark.xfail(strict=True)` with a reason citing
   this ADR. `strict=True` means the test fails the suite the moment the
   one-shot retrain lands real weights (`smoke: false` + a sidecar whose
   sha256 matches the shipped ONNX), forcing removal of the marker — so
   the deferral cannot silently outlive its cause.
3. When the one-shot retrain runs, it must re-run
   `export_ensemble_v2_seeds.py` so the ONNX bytes and sidecars
   regenerate together at `codec_vocab = 6`, then flip `smoke: false` and
   remove the xfail marker — exactly the workflow ADR-0321's follow-ups
   already mandate (hand-flipping rows remains forbidden).

This does not modify any Netflix golden-data assertion (CLAUDE.md §8);
`model_registry_schema_test.py` is a fork-local file.

## Alternatives considered

- **Retrain the five ensemble seeds now (production flip immediately).**
  This is the eventual correct end state but contradicts the locked
  one-shot retrain decision (no piecemeal retraining before the
  toolchain is RC-frozen) and would risk moving numbers the one-shot run
  is meant to freeze. Rejected for RC; it is precisely what the one-shot
  retrain will do.
- **Revert the ONNX to the old `[batch, 14]` production weights.**
  Restores `smoke: false` consistency but re-breaks the load path under
  the current `codec_vocab = 6`, reintroducing the
  `eval_probabilistic_proxy` failure #865 fixed. Rejected.
- **Leave the test failing as a known local red.** The schema test is
  non-gating in CI (it runs under the `|| true` block in
  `tests-and-quality-gates.yml`), so this would not break master. But a
  bare red is noise that can mask a future regression in the same test
  and carries no self-healing signal. The strict-xfail marker is the
  honest, self-documenting, auto-alerting representation. Rejected in
  favour of strict xfail.
- **Delete the stale per-seed sidecars.** They describe the older
  production weights, not the shipped smoke ONNX. Keeping them preserves
  the genuine `PROMOTE` provenance and the sha the one-shot retrain will
  supersede; the only consumer is the now-xfailed production test.
  Rejected (kept) to retain provenance.

## Consequences

- **Positive**: The RC registry is internally consistent and honest —
  every entry has license metadata, and `smoke: true` truthfully
  describes the shipped weights. The deferral is tracked by a strict
  marker that fails loudly when resolved.
- **Negative**: The probabilistic ensemble head ships at smoke quality in
  the RC. Any consumer that loads it gets placeholder predictions until
  the one-shot retrain. This is documented in the model card and state.md.
- **Neutral / follow-ups**: The one-shot retrain must (a) re-export the
  five seeds via `export_ensemble_v2_seeds.py` at `codec_vocab = 6`,
  (b) flip `smoke: false`, (c) remove the xfail marker in
  `model_registry_schema_test.py`. Tracked in `docs/state.md` and the
  retrain plan.

## Supply-chain impact

- **New dependencies**: none.
- **Removed dependencies**: none.
- **Build-time fetches**: none.

## References

- Parent / superseded-context: [ADR-0321](0321-fr-regressor-v2-ensemble-full-prod-flip.md),
  [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md),
  [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md).
- Regression source: PR #865 (`8b7ae731a`) — dropped license metadata and
  regenerated ONNX in `--smoke` mode.
- `req` — operator direction: retrain all models exactly once, after the
  toolchain reaches RC and feature numbers are frozen, to avoid repeating
  the retrain; the ensemble is in scope for that one-shot run.
