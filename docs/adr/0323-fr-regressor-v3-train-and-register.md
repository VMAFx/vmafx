# ADR-0323: `fr_regressor_v3` — train + register on ENCODER_VOCAB v3 (16-slot)

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, fr-regressor, codec-aware, encoder-vocab, loso, fork-local
- **Related**:
  [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) (v3 16-slot
  schema scaffold + ship gate),
  [ADR-0291](0291-fr-regressor-v2-prod-ship.md) (v2 production-flip;
  defines the 0.95 LOSO PLCC ship gate),
  [ADR-0235](0235-codec-aware-fr-regressor.md) (codec-aware FR
  regressor decision; ≥+0.005 PLCC multi-codec lift floor),
  [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) (LOSO trainer
  pattern reused),
  [Research-0078](../research/0078-encoder-vocab-v3-schema-expansion.md)
  (v3 schema expansion plan + retrain checklist)

## Context

PR #401 (ADR-0302) shipped `ENCODER_VOCAB_V3` as a parallel constant
in `ai/scripts/train_fr_regressor_v2.py` — 16 slots, append-only
extension of the production v2 vocab to cover three vmaf-tune codec
adapters that landed since `fr_regressor_v2` flipped to production
(`libsvtav1`, `h264_videotoolbox`, `hevc_videotoolbox`). The live
`ENCODER_VOCAB_VERSION = 2` remained authoritative.

The Phase A canonical-6 corpus
(`runs/phase_a/full_grid/per_frame_canonical6.jsonl`, 5,640 rows,
NVENC-only) is now generated locally via
`scripts/dev/hw_encoder_corpus.py`. The LOSO trainer pattern that
PR #422 (ADR-0319) implemented for the v2 ensemble is the natural
recipe to reuse for a v3 single-checkpoint training run gated on the
ADR-0302 / ADR-0291 ship floor (mean LOSO PLCC ≥ 0.95).

This PR closes the v3 retrain deferral by training a
`fr_regressor_v3.onnx` checkpoint and registering it as a parallel
model alongside `fr_regressor_v2`.

## Decision

**Train `fr_regressor_v3` on the existing NVENC-only Phase A corpus
using the v2 LOSO recipe; ship the model only if mean LOSO PLCC ≥
0.95. Do not flip `ENCODER_VOCAB_VERSION` from 2 to 3 in the v2
trainer — that's a separate "promote v3 to authoritative" PR.**

Concretely:

1. Add `ai/scripts/train_fr_regressor_v3.py`. The trainer imports
   `ENCODER_VOCAB_V3` from the v2 trainer (the constant landed in
   PR #401) and consumes the same Phase A canonical-6 JSONL the v2
   ensemble LOSO trainer (ADR-0319) consumes. Pipeline mirrors
   `train_fr_regressor_v2_ensemble_loso._load_corpus` /
   `_train_one_seed` / `_predict_fold`, with three structural
   changes:

   - 16-slot one-hot (`ENCODER_VOCAB_V3`) instead of v2's 12-slot.
   - 18-D codec block (`16 + preset_norm + crf_norm`) — the
     `--num-codecs` default is `CODEC_BLOCK_DIM = 18`.
   - Output tensor renamed `score → vmaf` to match the corpus
     teacher-score column.

2. Run the 9-fold LOSO gate at training time. If mean LOSO
   PLCC < 0.95, the trainer **exits non-zero** and writes the
   sidecar / registry row with `gate_passed: false` + `smoke: true`
   so the PR ships the scaffold + audit trail without claiming
   production status. On gate-pass, the trainer fits a final
   full-corpus checkpoint and exports it to
   `model/tiny/fr_regressor_v3.onnx` (opset 17, two-input
   `features:[N,6]` + `codec_block:[N,18]` → `vmaf:[N]`).

3. Sidecar JSON `model/tiny/fr_regressor_v3.json` mirrors the
   `fr_regressor_v2.json` shape with three additions:
   `encoder_vocab_version: 3`, `corpus`, `corpus_sha256`,
   `loso_mean_plcc`, `gate_passed`, full per-fold LOSO trace.

4. Registry entry `fr_regressor_v3` lands as a new row keyed
   alphabetically between `fr_regressor_v2_ensemble_v1_seed4` and
   `learned_filter_v1`. `smoke: false` on gate-pass; `smoke: true`
   on gate-fail with an explicit "v3 retrain pending" note.

5. Tests under `ai/tests/test_train_fr_regressor_v3.py` cover the
   loader contract on a 16-row synthetic fixture exercising 4 of the
   16 vocab slots (`libx264`, `h264_nvenc`, `libsvtav1`,
   `hevc_videotoolbox`), a 1-epoch full-corpus fit + ONNX export
   round-trip, and the LOSO summary schema. CPU-only, sub-second.

6. Doc card `docs/ai/models/fr_regressor_v3.md` documents the
   18-D codec block, the headline LOSO results, and — honestly — the
   NVENC-only corpus caveat (the 15 non-NVENC slots receive zero
   training rows and degrade to v1-baseline behaviour at inference
   time).

7. The ADR-0235 multi-codec lift floor (≥+0.005 PLCC over v1) is
   **deferred as not-yet-measurable** on this corpus drop: the
   NVENC-only corpus reduces v3-vs-v1 lift to a v1-vs-v1 comparison
   on a single codec. The follow-up PR that gates production-flip
   of `fr_regressor_v3.onnx` over `fr_regressor_v2.onnx` will
   measure the lift on a multi-codec corpus drop.

## Headline result

**Gate PASS.** Mean LOSO PLCC = 0.9975 ± 0.0018 across the 9 Netflix
sources, every source clears 0.99. Min/max PLCC spread 0.9945 →
0.9996 (well under ADR-0303's 0.005 ensemble-spread bound). Full
per-fold trace lives in
`docs/ai/models/fr_regressor_v3.md` §Headline results and in the
sidecar JSON `training.loso_folds`. Registry row lands with
`smoke: false`.

## Alternatives considered

### 1. Train now on the NVENC-only corpus (chosen)

Pros: closes ADR-0302 deferral; demonstrates the v3 schema works
end-to-end with real-weight ONNX; unblocks downstream work that
wants a v3 sidecar to test against; sidecar format is the
forward-compatible shape future multi-codec retrains will adopt.
Cons: 15 of 16 vocab slots receive zero training data, so v3's
behaviour on non-NVENC codecs is essentially v1-baseline. The
ADR-0235 multi-codec lift floor is unmeasurable on this corpus.

### 2. Wait for a multi-codec corpus drop

Pros: would let v3 train against ≥3 codec families and measure the
multi-codec lift floor honestly; matches the spirit of ADR-0235's
"never silently default to a codec that doesn't match what the
script actually encoded" rule. Cons: the multi-codec corpus drop
has been blocking on Phase A runner expansion since PR #283
landed (~3 months); v3 has been in scaffold-only limbo since
PR #401. The corpus blocker is independent of the v3 schema
scaffold; gating one on the other compounds backlog. Mitigation
chosen instead: ship v3 now with explicit caveats; defer the
production-flip (v2.onnx → v3.onnx slot reassignment) until a
multi-codec corpus exists.

### 3. Scaffold-only with `smoke: true`

Pros: zero risk of shipping a model that misleads consumers about
its multi-codec coverage. Cons: identical to PR #401's status quo;
ships no new artefact for downstream PRs to consume; no LOSO gate
result captured. Rejected because the trainer's gate gives us a
real PLCC number to anchor future retrain comparisons against, even
on a single-codec corpus.

## Consequences

- A new `fr_regressor_v3.onnx` ships under `model/tiny/`, registered
  with `smoke: false`. The libvmaf DNN runtime can load it via the
  registry but has no caller wired to consume it yet — the
  production consumer (`vmaf-tune fast`) still consumes
  `fr_regressor_v2.onnx` per ADR-0304.
- `fr_regressor_v2.onnx` and `fr_regressor_v2.json` are
  **untouched** (per the task brief constraint). The append-only
  vocab invariant is honoured.
- The v3 16-slot schema is now exercised end-to-end against a real
  ONNX export + ORT round-trip. Any downstream PR that wants to
  validate v3-shape inference can use this checkpoint.
- The NVENC-only caveat is documented in the model card, sidecar
  notes, and this ADR. The follow-up retrain PR (gated on a
  multi-codec corpus) will lift the caveat.

## Reproducer

```bash
# Real corpus (the Phase A NVENC-only drop):
python ai/scripts/train_fr_regressor_v3.py \
    --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl

# Smoke (synthetic, sub-second):
python ai/scripts/train_fr_regressor_v3.py --smoke

# Tests + registry verification:
pytest ai/tests/test_train_fr_regressor_v3.py -v
bash core/test/dnn/test_registry.sh
```

## References

- `req` 2026-05-06: "Open a draft PR titled `feat(ai):
  fr_regressor_v3 — train + register on ENCODER_VOCAB v3 (16-slot)`
  against `VMAFx/vmafx` master." (paraphrased; this PR's task
  brief).
- [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) §Retrain
  ship gate — re-uses ADR-0291's mean LOSO PLCC ≥ 0.95 floor.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) §Decision —
  baseline v2 production-flip cleared at 0.9681.
- [ADR-0235](0235-codec-aware-fr-regressor.md) §References —
  append-only vocab invariant + +0.005 PLCC multi-codec lift floor.
- [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) §Decision —
  LOSO recipe (Adam(lr=5e-4, wd=1e-5), 200 epochs, bs=32,
  fold-local StandardScaler).
- [Research-0078](../research/0078-encoder-vocab-v3-schema-expansion.md)
  §Production-flip checklist — items this PR's follow-up retrain PR
  is responsible for (the v2 → v3 in-place swap).
