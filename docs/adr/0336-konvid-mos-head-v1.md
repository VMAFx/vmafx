<!-- markdownlint-disable MD060 -->
# ADR-0336: KonViD MOS head v1 (ADR-0325 Phase 3)

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `model/konvid_mos_head_v1.onnx` and model card present on master;
  MOS surface exposed via `Predictor.predict_mos()`.
- **Date**: 2026-05-08
- **Deciders**: @Lusoris
- **Tags**: ai, training, mos, konvid, fork-local

## Status update (2026-05-15)

The corpus-availability blocker called out in the original Context is
REMOVED — `.workingdir2/konvid-150k/` materialized 2026-05-09 with the
full 179 GB corpus (307 682 clips, k150ka/k150kb scores CSV, JSONL,
manifest). Per [ADR-0325](0325-konvid-150k-corpus-ingestion.md) status
update of the same date.

The remaining gate is the real-corpus PLCC verification:
`train_konvid_mos_head.py` against the materialized corpus must clear
`PLCC ≥ 0.85` mean, `≤ 0.005` spread, `SROCC ≥ 0.82`,
`RMSE ≤ 0.45`. Tracked as Batch 22 of
`.workingdir/GAP-FILL-PLAN-2026-05-15.md`; runs after the in-flight CHUG
feature extraction releases the GPU (~10 h ETA from 2026-05-15
21:00 local).

Status stays `Proposed` until the production-flip gate clears.

## Context

[ADR-0325](0325-konvid-150k-corpus-ingestion.md) plans a three-phase
adoption of the Konstanz UGC corpora as a third training shard for the
fork's prediction stack. Phase 1 (KonViD-1k, [PR #440]) and Phase 2
(KonViD-150k, [PR #447] in flight) ingest the corpora; Phase 3 — this
ADR — trains a head against the resulting subjective-MOS labels.

The fork's existing prediction surface
([`fr_regressor_v2_ensemble`](fr_regressor_v2_ensemble_v1) /
[`fr_regressor_v3`](0323-fr-regressor-v3-train-and-register.md) /
[`nr_metric_v1`](0168-tinyai-konvid-baselines.md)) targets *VMAF*, not
raw subjective MOS. The ChatGPT-vision audit ([Research-0086]) flagged
this as a structural gap: open-weight competitors (DOVER-Mobile @ PLCC
0.853 KoNViD, Q-Align) ship MOS predictors directly, and the fork
cannot honestly claim "we predict human MOS" without a head trained
against subjective ratings. KonViD ships ≥5 crowdworker MOS ratings per
clip, which is exactly the subjective ground truth the head needs.

## Decision

We will train and register `konvid_mos_head_v1`, a small MLP that maps
the canonical-6 libvmaf features + saliency mean/var + 3 TransNet
shot-metadata columns + a single-slot ENCODER_VOCAB v4 one-hot to a
scalar MOS prediction in `[1.0, 5.0]`. The trainer is
[`ai/scripts/train_konvid_mos_head.py`](../../ai/scripts/train_konvid_mos_head.py);
the shipped artefacts are
[`model/konvid_mos_head_v1.onnx`](../../model/konvid_mos_head_v1.onnx)
and the model card
[`model/konvid_mos_head_v1_card.md`](../../model/konvid_mos_head_v1_card.md).
The MOS surface is exposed to vmaf-tune callers via
`Predictor.predict_mos(features, codec)` in
[`tools/vmaf-tune/src/vmaftune/predictor.py`](../../tools/vmaf-tune/src/vmaftune/predictor.py)
with a documented linear approximation
(`mos = (predicted_vmaf - 30) / 14`, clamped to `[1, 5]`) as the
fallback when the ONNX is missing.

The production-flip gate (`mean PLCC ≥ 0.85`, `mean SROCC ≥ 0.82`,
`mean RMSE ≤ 0.45`, `spread ≤ 0.005`) is not met by the synthetic-
corpus checkpoint shipped in this PR; per the user direction (memory
`feedback_no_test_weakening`) the threshold is **not** lowered. The
checkpoint ships with `Status: Proposed` instead, with the gate
verdict recorded in the model card. The head flips to `Status:
Accepted` when the real KonViD-150k retrain (gated on PR #447 landing)
clears the gate.

## Alternatives considered

| Option                                                | Pros                                                     | Cons                                                                  | Why not chosen                                                                                                                                                              |
|-------------------------------------------------------|----------------------------------------------------------|-----------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Ship the MOS head**                                 | Honest MOS prediction surface; matches DOVER-Mobile et al. | Real-corpus retrain blocked on PR #447; synthetic-only artefact today | Chosen. ADR-0325's phased rollout already accepted that Phase 3 lands behind a `Proposed` checkpoint until the corpus arrives; the predictor surface is forward-compatible. |
| **Reuse `fr_regressor_v2_ensemble` with linear remap** | No new ONNX; reuses an already-shipped graph             | Mis-represents fork capability — VMAF and MOS are not interchangeable | Insufficient on its own, but kept as the *fallback* path in the predictor so callers without the head still get a plausible 5-point estimate.                              |
| **Defer Phase 3 until real corpus is on disk**        | Avoids carrying a `Proposed` checkpoint                  | Predictor surface stays incomplete; PR #447 is still in flight        | Rejected — the ENCODER_VOCAB v4 single-slot collapse + the fallback path are independently useful, and the ADR-0325 §Phase 3 plan committed to landing the head shape now.  |
| **Train against IQA-Lab Q-Align / DOVER weights**     | Pre-trained on a comparable dataset                      | Imports an external model + breaks ADR-0325's "fork-redistributes ONNX only" rule | Out of scope. Q-Align is too large; DOVER-Mobile is fine but doesn't satisfy ADR-0258 op-allowlist conformance without adapter work.                                          |

## Consequences

- **Positive**:
  - The fork ships a head trained against subjective MOS for the first
    time; the user-facing claim "we predict human MOS" becomes
    defensible (with the `Proposed` qualifier on the synthetic-corpus
    checkpoint).
  - `Predictor.predict_mos` adds an explicit MOS surface to vmaf-tune
    that is forward-compatible with future multi-corpus expansion
    (LSVQ, YouTube-UGC, etc.) — the ENCODER_VOCAB v4 one-hot is sized
    so adding slots is an append-only schema bump.
  - The deterministic-seeded smoke run produces a bit-identical ONNX
    on every machine, which makes the gate-test harness reliable in
    CI.

- **Negative**:
  - The shipped checkpoint is a synthetic-corpus surrogate; the
    production flip is gated on PR #447 + a real-corpus retrain.
    Anyone consulting the model card today will see an honest
    `Proposed` status with the synthetic gate verdict; that's
    deliberate (memory `feedback_no_test_weakening`).
  - The TransNet shot-metadata columns referenced in the feature
    layout are not yet emitted by the KonViD ingester (PR #477 will
    bolt them on); until then the MOS head zero-fills those columns
    and runs effectively without shot-context features.

- **Neutral / follow-ups**:
  - When PR #447 lands and the user provisions
    `~/.workingdir2/konvid-150k/konvid_150k.jsonl`, re-run the
    trainer without `--smoke`; if the gate clears, flip the model
    card from `Proposed` to `Accepted` and update
    [`docs/state.md`](../state.md).
  - When PR #477 lands, regenerate the corpus with shot-metadata
    columns and re-train under the same seed.
  - Future ENCODER_VOCAB v4 expansion (LSVQ, YouTube-UGC) is an
    append-only schema bump; existing trained ONNX stays loadable.

## References

- [ADR-0325](0325-konvid-150k-corpus-ingestion.md) — parent corpus-
  ingestion ADR; defines the production-flip gate this ADR mirrors.
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) —
  production-flip protocol shape this gate inherits.
- [ADR-0258](0258-onnx-allowlist-resize.md) — ONNX op-allowlist this
  graph conforms to.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) —
  `saliency_student_v1`, the source of the saliency_mean / saliency_var
  feature columns.
- [ADR-0223](0223-transnet-v2-shot-detector.md) — TransNet v2 shot
  detector, the source of the 3 shot-metadata columns.
- [PR #440] — KonViD-1k Phase 1 ingestion.
- [PR #447] — KonViD-150k Phase 2 ingestion (in flight).
- [Research-0086](../research/0086-konvid-150k-corpus-feasibility.md)
  — KonViD-150k corpus feasibility audit.
- [Research-0090](../research/0090-konvid-mos-head-design.md) — design
  notes for this head (this PR).
- Source: `req` — direct user implementation task brief
  ("train a fork-owned tiny **MOS head** that predicts subjective MOS
  …").

[PR #440]: https://github.com/VMAFx/vmafx/pull/440
[PR #447]: https://github.com/VMAFx/vmafx/pull/447
[Research-0086]: ../research/0086-konvid-150k-corpus-feasibility.md
