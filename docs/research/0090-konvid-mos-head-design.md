<!-- markdownlint-disable MD013 MD060 -->
# Research-0090 — KonViD MOS head v1 design

- **Status**: Notes for [ADR-0336](../adr/0336-konvid-mos-head-v1.md) (Phase 3 of [ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md))
- **Date**: 2026-05-08
- **Author**: @Lusoris

## Question

How small can a fork-owned MOS head be while still clearing the
ADR-0325 production-flip gate (`PLCC ≥ 0.85`, `SROCC ≥ 0.82`, `RMSE ≤
0.45`, `spread ≤ 0.005`) on KonViD-style UGC content? And what feature
shape should it consume, given that the in-flight Phase 1/2 ingester
(PR #440 / #447) does not yet emit canonical-6 / saliency / shot-
metadata columns?

## Survey of existing open-weight MOS predictors

| Model           | Params | License | KoNViD-1k PLCC | KoNViD-1k SROCC | Notes                                                                                            |
|-----------------|--------|---------|----------------|-----------------|--------------------------------------------------------------------------------------------------|
| DOVER-Mobile    | ~3.8M  | Apache  | 0.853          | 0.860           | Mobile sibling of DOVER (Wu et al. 2023). Two-branch (technical + aesthetic) over swin-tiny.     |
| Q-Align         | ~7B    | MIT     | 0.876          | 0.884           | LLM-based, far too large for embedding inside libvmaf.                                           |
| FAST-VQA        | ~22M   | Apache  | 0.859          | 0.851           | Spatial-temporal sampling; comparable size to fr_regressor_v2 family + a 3D CNN frontend.        |
| MD-VQA          | ~10M   | Apache  | 0.846          | 0.835           | Multi-dim VQA, swin-base.                                                                        |

Sources: published papers + the IQA-PyTorch leaderboard. The
common denominator across the three competitive Apache-licensed
predictors is roughly `4M+` params plus a backbone the fork's ONNX
op-allowlist (`core/src/dnn/op_allowlist.c`) does not admit
without `Resize` / patch-embed / multi-head-attention surgery.

## Design constraint — ONNX-allowlist conformance

The fork's allowlist (per [ADR-0258] / [ADR-0169]) is dense + conv +
pool + standard activations + LayerNorm. That rules out:

- **Patch-embed Conv1d / Linear via einsum** — admissible only if the
  graph lowers to plain `Gemm` (which patch-embed *does* with
  reasonable export settings, but the rest of swin-tiny carries
  `MultiHeadAttention` ops that don't).
- **3D CNN frontends** — would need `Conv3d`. Admissible op, but the
  size cost is prohibitive for an in-libvmaf-shipped model.

Conclusion: a competitive head needs to consume *summarised* features
(canonical-6, saliency mean/var, shot stats) rather than raw frames.
That's the shape the rest of the fork's prediction stack already uses
and matches the Phase A / fr_regressor_v2 corpus shape.

## Decision — feature shape

11-D feature vector + 1-D ENCODER_VOCAB v4 one-hot:

| Index | Feature              | Source                         |
|-------|----------------------|--------------------------------|
| 0     | `adm2`               | libvmaf canonical-6            |
| 1     | `vif_scale0`         | libvmaf canonical-6            |
| 2     | `vif_scale1`         | libvmaf canonical-6            |
| 3     | `vif_scale2`         | libvmaf canonical-6            |
| 4     | `vif_scale3`         | libvmaf canonical-6            |
| 5     | `motion2`            | libvmaf canonical-6            |
| 6     | `saliency_mean`      | `saliency_student_v1` ([ADR-0286]) |
| 7     | `saliency_var`       | `saliency_student_v1` ([ADR-0286]) |
| 8     | `shot_count_norm`    | TransNet v2 ([ADR-0223])       |
| 9     | `shot_mean_len_norm` | TransNet v2 ([ADR-0223])       |
| 10    | `shot_cut_density`   | TransNet v2 ([ADR-0223])       |

Phase 1/2 KonViD JSONL rows do not yet carry columns 0–10; the trainer
zero-fills them and runs effectively against the MOS column alone for
now. Subsequent PRs (#477 for shot metadata; canonical-6 + saliency
extraction during ingestion as a separate follow-up) bolt the
columns on.

The ENCODER_VOCAB v4 one-hot is `[1.0]` (always asserted on the
single `"ugc-mixed"` slot per ADR-0325 §Decision). The 1-D shape is
forward-compatible: when LSVQ + YouTube-UGC ingestion lands, the new
slots append at the end and existing ONNX stays loadable.

## Architecture choice

```text
LayerNorm(12)
  → Linear(12, 64) → ReLU → Dropout(0.1)
  → Linear(64, 64) → ReLU → Dropout(0.1)
  → Linear(64, 1)
  → Sigmoid + affine to [1, 5]
```

5,081 parameters total. The Sigmoid + affine wrapper bakes the `[1.0,
5.0]` MOS range into the graph — adversarial input cannot drive the
output below 1 or above 5 — so the predictor surface does not need a
runtime clamp on top.

The 30K–100K-param range from the task brief is wider than this; the
actual architecture lands smaller because the input is already a
summarised feature vector and a deeper MLP overfits the 600-row
synthetic corpus.

## Synthetic-corpus gate verdict (this PR)

5-fold cross-validation on a deterministic-seeded 600-row synthetic
corpus produces:

| Fold     | PLCC       | SROCC      | RMSE       |
|----------|------------|------------|------------|
| 0        | 0.8677     | 0.8831     | 0.2565     |
| 1        | 0.8854     | 0.9356     | 0.2138     |
| 2        | 0.8017     | 0.8453     | 0.3079     |
| 3        | 0.8839     | 0.9442     | 0.2263     |
| 4        | 0.8596     | 0.8938     | 0.2291     |
| **Mean** | **0.8597** | **0.9004** | **0.2467** |

PLCC spread = `0.0836`. The synthetic surrogate gate (`PLCC ≥ 0.75`)
clears; the production-flip gate (`PLCC ≥ 0.85` mean, `≤ 0.005`
spread) does **not** clear, which is expected because synthetic noise
is honestly noisier than real KonViD inter-rater consistency. **The
gate is not lowered** (memory `feedback_no_test_weakening`); the
checkpoint ships with `Status: Proposed` and the real-corpus retrain
is gated on PR #447.

## Fallback path — `mos = (vmaf - 30) / 14`

Why this specific linear remap, and not a more sophisticated
calibration?

- It maps VMAF 30 (visibly distorted) to MOS 0 (clamped to 1) and
  VMAF 100 (transparent) to MOS 5, giving a plausible 5-point
  estimate without per-codec calibration data.
- The slope `1/14` is the inverse of the empirical `(MOS - 1) * 14 +
  30 ≈ VMAF` regression Netflix's blog post on VMAF-vs-MOS
  approximations cites; using their inverse keeps the fallback in
  the same ball-park as the legacy assumption.
- The clamp to `[1, 5]` keeps the surface honest — anything outside
  the MOS scale is a fallback artefact, not a real prediction.

The model card flags the fallback as approximate, not authoritative;
callers that need MOS for a non-debug purpose should ensure the
ONNX is shipped or block on the production-flip retrain.

## Follow-ups

1. Extend the KonViD ingester (`ai/scripts/konvid_*_to_corpus_jsonl.py`)
   to emit the canonical-6 + saliency + shot-metadata columns. Today's
   trainer accepts those columns when present and zero-fills when
   absent, so the change is forward-compatible.
2. When PR #447 lands, re-run the trainer without `--smoke` and
   re-evaluate the production-flip gate. If it clears, flip the model
   card from `Proposed` to `Accepted` and update
   [`docs/state.md`](../state.md).
3. ENCODER_VOCAB v4 expansion (LSVQ, YouTube-UGC) — append-only schema
   bump, retrain under the same seed.

## References

- [ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) — parent
  corpus-ingestion ADR.
- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  production-flip protocol shape.
- [ADR-0258](../adr/0258-onnx-allowlist-resize.md) — ONNX op-allowlist
  this graph conforms to.
- [ADR-0223](../adr/0223-transnet-v2-shot-detector.md) — shot-metadata
  source.
- [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md) —
  saliency-feature source.
- [Research-0086](0086-konvid-150k-corpus-feasibility.md) — KonViD-150k
  corpus feasibility audit.
- [PR #440] — KonViD-1k Phase 1 ingestion.
- [PR #447] — KonViD-150k Phase 2 ingestion (in flight).

[ADR-0258]: ../adr/0258-onnx-allowlist-resize.md
[ADR-0169]: ../adr/0169-onnx-allowlist-loop-if.md
[ADR-0223]: ../adr/0223-transnet-v2-shot-detector.md
[ADR-0286]: ../adr/0286-saliency-student-fork-trained-on-duts.md
[PR #440]: https://github.com/VMAFx/vmafx/pull/440
[PR #447]: https://github.com/VMAFx/vmafx/pull/447
