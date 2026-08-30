<!-- markdownlint-disable MD060 -->
# ADR-0320: `fr_regressor_v2` ensemble seeds — production flip (smoke → false)

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, fr-regressor, ensemble, registry, prod-flip, fork-local
- **Related**: [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md)
  (gate definition: mean ≥ 0.95 AND spread ≤ 0.005),
  [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  (real-corpus retrain harness + validator),
  [ADR-0319](0319-ensemble-loso-trainer-real-impl.md)
  (real `_load_corpus` + `_train_one_seed` implementation)

## Context

[ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) defined the
two-part production-flip gate for the `fr_regressor_v2_ensemble_v1`
deep ensemble: **mean per-seed LOSO PLCC ≥ 0.95** AND **per-seed
spread `max - min` ≤ 0.005**. ADR-0309 shipped the harness wrapper +
`ai/scripts/validate_ensemble_seeds.py` that emits a `PROMOTE.json`
verdict. ADR-0319 (PR #422, merged 2026-05-06) plugged in the real
loader + per-fold trainer body, and the operator ran the harness
end-to-end against the locally-generated Phase A canonical-6 corpus
(5,640 rows from 9 Netflix sources × `h264_nvenc` × 4 CQs).

The verdict file `runs/ensemble_v2_real/PROMOTE.json` reports:

- `mean_plcc = 0.9972533887602454` (gate `≥ 0.95` ✓)
- `plcc_spread = 0.0009510602756565012` (gate `≤ 0.005` ✓)
- per-seed PLCC range `[0.9968, 0.9978]`, no failing seeds.

Both gate components pass with substantial margin. Per ADR-0303's
contract, this is the trigger to flip the five
`fr_regressor_v2_ensemble_v1_seed{0..4}` rows from `smoke: true` to
`smoke: false` — promoting them from "scaffolded" to "production-eligible".

## Decision

**Flip the five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
`model/tiny/registry.json` from `smoke: true` to `smoke: false`,
commit the PROMOTE.json verdict alongside the registry mutation as
the audit trail, and update `ai/AGENTS.md` to record the flip.**

The verdict file is committed at
`model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json` so
future readers can reconstruct the gate result without re-running
the trainer. No aggregate `fr_regressor_v2_ensemble_v1_mean` row
exists in the registry today; if one is registered later, ADR-0303's
"flip only after all five seeds clear *and* the variance bound holds"
clause already applies and is satisfied by this verdict.

The trainer (`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`),
the validator (`ai/scripts/validate_ensemble_seeds.py`), and the
gate (`scripts/ci/ensemble_prod_gate.py`) are **not** modified —
ADRs 0303 / 0309 / 0319 are the contract this flip honours.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Flip now (chosen)** | Honours ADR-0303's gate contract immediately; both gate components pass with substantial margin (mean PLCC 0.9973 vs 0.95 threshold; spread 0.00095 vs 0.005 threshold); unblocks the in-flight `vmaf-tune --quality-confidence` consumer (ADR-0237) which needs production-eligible ensemble seeds; verdict file is reproducible from the harness. | The corpus is NVENC-only (`h264_nvenc` × 4 CQs); QSV / AMF / VideoToolbox seeds were not exercised. | Selected — gate is corpus-agnostic by design; ADR-0303's contract makes no demand on encoder coverage at flip time. Encoder-coverage gaps are a follow-up backlog item, not a flip blocker. |
| Re-run with QSV first | Broader hardware-encoder coverage before promotion; would catch any QSV-specific PLCC degradation. | The gate threshold is per-seed mean PLCC, not per-encoder; ADR-0303's two-part gate is already satisfied; deferring promotion to chase additional encoder coverage adds wall-time without changing the gate-pass verdict. | Rejected — ADR-0303's gate is what it is; chasing additional encoder coverage is scope-creep against the established contract. Tracked as follow-up T-row. |
| Wait for BVI-DVC corpus expansion (ADR-0310) | Larger / more diverse corpus would tighten the calibration story. | BVI-DVC corpus ingestion is gated on dataset access negotiations (open-ended); deferring flip indefinitely punishes the `vmaf-tune` consumer for an external-trigger event. | Rejected — corpus expansion is independent of the promotion gate. When BVI-DVC lands, a fresh PROMOTE.json + new flip ADR (or supersedure of this one) is the right path, not blocking on it now. |

## Consequences

- **Positive**: the five ensemble seeds are now production-eligible
  per the registry's `smoke: false` semantics. Downstream consumers
  (`vmaf-tune --quality-confidence`, ADR-0237 Phase A) can rely on
  the ensemble for predictive-distribution queries.
- **Positive**: the verdict file
  (`model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json`)
  is in-tree and immutable, so the gate-pass record survives without
  the runtime artefacts under `runs/`.
- **Negative**: any future change to these registry rows (sha256
  bump after retraining, description update beyond cosmetic) now
  requires a fresh PROMOTE.json verdict per ADR-0303's gate. This
  is recorded as the going-forward invariant in `ai/AGENTS.md`.
- **Neutral / follow-up**: aggregate `fr_regressor_v2_ensemble_v1_mean`
  registry entry can be added in a follow-up PR if a single-row
  consumer-facing alias is needed; ADR-0303's variance-bound clause
  already governs that flip and is satisfied by this verdict.
- **Neutral / follow-up**: encoder-coverage expansion (QSV / AMF /
  VideoToolbox corpora) remains backlog. A fresh PROMOTE.json on a
  broader corpus would tighten confidence in the cross-encoder
  calibration but is not a flip-blocker per ADR-0303.

## References

- req (2026-05-06, operator): "PR #422 (ADR-0319) plugged in the real
  loader + trainer; the operator ran the harness end-to-end and got a
  PROMOTE verdict. Per ADR-0303 this is the trigger to flip the five
  ensemble seed rows from `smoke: true` to `smoke: false`."
  (paraphrased from the dispatcher prompt requesting registry flip.)
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — defines the
  two-part flip gate this PR honours (mean PLCC ≥ 0.95 AND spread ≤ 0.005).
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  defines the harness + verdict-file contract this PR cites.
- [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) — shipped the
  real loader + per-fold trainer body (PR #422) that produced the
  verdict file referenced here.
- `model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json` —
  the gate-pass verdict committed alongside this flip.
- [`scripts/ci/ensemble_prod_gate.py`](../../scripts/ci/ensemble_prod_gate.py)
  — single source of truth for the gate thresholds.
- [`ai/scripts/validate_ensemble_seeds.py`](../../ai/scripts/validate_ensemble_seeds.py)
  — emitter of the PROMOTE.json schema.
