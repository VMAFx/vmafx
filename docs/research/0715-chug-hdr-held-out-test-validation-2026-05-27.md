<!-- markdownlint-disable MD060 -->
# Research 0715: CHUG HDR MOS Head — Held-Out Test Partition Results (2026-05-27)

## Question

What are the unbiased generalisation metrics for `chug_hdr_mos_head_v1_wide_seed20260521`
on the 552-row held-out test partition?

## Setup

- **Model**: `chug_hdr_mos_head_v1_wide_seed20260521.onnx` (34 input features,
  `chug-hdr-wide-v1` schema, ENCODER_VOCAB v4 single slot).
- **Feature schema**: `chug-hdr-wide-v1` — canonical-6 means + p10/p90/std
  temporals + 10 HDR/geometry metadata columns (34 total).
- **Data**: CHUG feature JSONL shards at
  `.corpus/chug/training/fr_canonical_shards/output/shard_*.features.jsonl`
  (5136 total rows: 3936 train / 648 val / 552 test).
- **Evaluation**: `ai/scripts/validate_chug_hdr_mos_head.py` filtering to
  `split == "test"` rows; ONNX inference via `onnxruntime` CPU provider.

## Results

| Metric | Val (training-time) | Test (held-out) | Gate |
|--------|---------------------|-----------------|------|
| PLCC   | 0.8733              | **0.8468**      | ≥ 0.85 — **FAIL** |
| SROCC  | 0.8528              | **0.8188**      | ≥ 0.82 — **FAIL** |
| RMSE   | 0.2512              | **0.2639**      | ≤ 0.45 — OK |

Exit code: 2 (gate FAIL).

## Interpretation

Both PLCC and SROCC miss the production-flip gate. The gaps are small
(PLCC −0.027, SROCC −0.034) and RMSE is well within the threshold (0.2639 vs
limit 0.45), suggesting the model has useful predictive power but is not
ready for production promotion.

The val → test degradation (PLCC 0.8733 → 0.8468) is consistent with mild
over-selection bias: the best seed was chosen on the `val` split, so `val`
performance is optimistically biased relative to an unseen partition.

## Candidate remedies

1. **Retrain ship checkpoint on train+val**: The current checkpoint was trained
   on `train` only (matching the validation split procedure). Retraining the
   final checkpoint on `train+val` combined before running the held-out test
   should recover some of the generalisation gap.
2. **Saliency and display-profile features**: Adding
   `saliency_mean`/`saliency_var` (T-CHUG-DISPLAY-PROFILE-TRAINING-2026-05-20)
   may increase effective signal dimensionality for content-aware MOS prediction.
3. **Additional CHUG extraction batches**: More rows narrow the variance of the
   seed-selection bias.

## Conclusion

Model status remains `Proposed`. The first unbiased held-out evaluation is
recorded; gate thresholds are unchanged per ADR-0325 / `feedback_no_test_weakening`.
