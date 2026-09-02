<!-- markdownlint-disable MD060 -->
# ADR-0291: fr_regressor_v2 — flip from smoke to production

- **Status**: Accepted
- **Date**: 2026-05-05
- **Companion research digest**: [Research-0067](../research/0067-fr-regressor-v2-prod-loso.md)
- **Supersedes**: ADR-0272 (smoke scaffold)
- **Related**: ADR-0235 (codec-aware decision + 0.95 LOSO PLCC ship gate),
  ADR-0237 (Phase A corpus contract)

## Context

ADR-0272 shipped `fr_regressor_v2` as a smoke checkpoint pending a real
Phase A corpus. PR #392 produced the corpus (`hw_encoder_corpus.py`,
33,840 per-frame canonical-6 rows across 9 Netflix sources × NVENC + QSV).
PR #394 widened the encoder vocab to v2 (12 encoders). Together they
unblock training a production checkpoint that clears the ADR-0235 ship
gate (LOSO PLCC ≥ 0.95).

## Decision

Flip `model/tiny/registry.json` `fr_regressor_v2` row from `smoke: true`
to `smoke: false`, ship the trained ONNX (sha256
`67934b0b61c73eb852d84ffb34e3333756e8da2530179ecc830336133e63e69e`,
13,674 bytes), and freeze the input contract:

- **Input shape**: 6 canonical libvmaf features (adm2, vif_scale0..3,
  motion2, StandardScaler-normalised) + 14-D codec block (12-way encoder
  one-hot per ENCODER_VOCAB v2 + preset_norm + crf_norm).
- **Output**: scalar VMAF teacher score.
- **MLP shape**: `6 → 32 → 32 → 32 → 1` with codec block concatenated
  before the first dense layer (matches FRRegressor `num_codecs=12`).
- **Production default** stays `vmaf_tiny_v2` per the tiny-AI ladder;
  `fr_regressor_v2` is a teacher-score predictor for vmaf-tune Phase B+
  workflows, not the runtime VMAF replacement.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| Ship at LOSO PLCC 0.9681 (chosen) | Clears 0.95 gate; ready now; corpus available | OldTownCross outlier (0.9183); SW encoders not in training set | Selected — gate cleared, caveats documented |
| Wait for SW-encoder corpus | Wider generalisation | Blocks vmaf-tune Phase B until SW-sweep ships (~hours) | Rejected — gate clears now, SW sweep tracked as T-FR-V2-SW-CORPUS |
| Larger MLP (64-64-64-1) | Higher in-sample fit | Marginal LOSO gain on 216 cells; overfit risk | Rejected — 32-32-32 fits 0.95 with margin |
| Per-frame regressor instead of per-cell | Uses all 33,840 rows | Wrong contract — fr_regressor_v2 emits per-cell teacher score | Rejected — per-frame is `vmaf_tiny_v3/v4` territory |

## Consequences

- **Visible behaviour**: callers that load `fr_regressor_v2` via the
  registry now get a real model, not a smoke graph. Existing
  `vmaf-tune` Phase A consumers (ADR-0237) gain a usable teacher
  predictor without re-encoding.
- **Backlog cleared**: removes "smoke-only fr_regressor_v2" from the
  smoke-checkpoint list.
- **Open follow-up**: T-FR-V2-SW-CORPUS — re-train once the
  software-encoder sweep produces canonical-6 features for x264/x265/
  svtav1/vvenc/vpx-vp9.

## References

- req (2026-05-05, popup): "v2-PROD trained — LOSO PLCC=0.9681 PASS.
  What now?" → "Both — ship v2-PROD + continue queue" (selected).
- [Research-0067](../research/0067-fr-regressor-v2-prod-loso.md)
  — LOSO eval results, reproducer, caveats.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — defines the
  0.95 LOSO PLCC ship gate.
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) — smoke
  scaffold this ADR supersedes.
- [ADR-0237](0237-quality-aware-encode-automation.md) — Phase A
  corpus contract.
