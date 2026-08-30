<!-- markdownlint-disable MD013 MD060 -->
# ADR-0687: CHUG HDR MOS head — held-out test partition validator

- **Status**: Accepted
- **Date**: 2026-05-27
- **Deciders**: lusoris
- **Tags**: ai, chug, mos-head, validation, fork-local

## Context

The CHUG corpus (`ai/scripts/train_chug_hdr_mos_head.py`) has explicit
`train` (3936), `val` (648), and `test` (552) splits in each feature JSONL
row's `split` column. Training and cross-validation use only `train` + `val`
rows. The `test` partition is held out by design so that at least one
unbiased evaluation remains after the model is selected.

The `chug_hdr_mos_head_v1_wide_seed20260521` checkpoint reported PLCC 0.8733
/ SROCC 0.8528 on the `val` split but had never been evaluated against `test`.
Before promoting the model or starting a new training pass, the unbiased
test-partition result is required to understand the true generalisation gap.

Per ADR-0325 the production-flip gate is PLCC ≥ 0.85, SROCC ≥ 0.82,
RMSE ≤ 0.45 MOS units. Per the `feedback_no_test_weakening` rule, the gate
thresholds are never lowered to accommodate a miss.

## Decision

We will add `ai/scripts/validate_chug_hdr_mos_head.py`, a standalone
validator that:

1. Loads a CHUG MOS head ONNX (default: the current best checkpoint).
2. Reads CHUG feature JSONL shards and filters to `split == "test"` rows.
3. Runs ONNX inference using `onnxruntime` (CPU provider).
4. Computes PLCC, SROCC, and RMSE against the `mos` column.
5. Emits a JSON report, a Markdown report, and a `write_run_manifest` sidecar.
6. Exits 0 on gate PASS, 2 on gate FAIL, 1 on input error.

The script reuses `_row_to_features`, `_load_jsonl`, `_normalise_split`, and
schema constants from `train_konvid_mos_head.py` so the feature projection is
byte-identical to the training path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add a `--evaluate-test` flag to the trainer | Single entry point | Training loop rerun required; gates would need careful separation from val-gate logic | Separation of concerns: a validator should not retrain |
| Jupyter notebook | Interactive exploration | Not scriptable, not CI-able, no run-manifest | Repository tooling requires reusable scripts |
| Inline ad-hoc evaluation in OPEN.md | Zero code | Not reproducible; no run-manifest; not tested | Must be reproducible and provenance-tracked |

## Consequences

- **Positive**: First unbiased held-out result for the CHUG HDR MOS head.
  Reproducible via a single command. Emits a provenance-tracked JSON + Markdown
  report. 19 unit tests cover split filtering, metric computation, and gate logic.
- **Negative**: PLCC 0.8468 / SROCC 0.8188 on the held-out `test` partition
  — both miss the ADR-0325 gate (PLCC short by 0.003, SROCC short by 0.001).
  Model ships `Status: Proposed` only; not promoted to production.
- **Neutral / follow-ups**: The held-out gap (val PLCC 0.8733 → test PLCC
  0.8468) is consistent with mild overfitting to the `val` distribution during
  seed-sweep model selection. Remedies to investigate: (1) reseed with
  train+val combined for the ship checkpoint; (2) add saliency or display-profile
  features (T-CHUG-DISPLAY-PROFILE-TRAINING-2026-05-20); (3) increase training
  epochs; (4) add more CHUG rows once future extraction batches complete.

## References

- ADR-0325 §Production-flip gate: PLCC ≥ 0.85 / SROCC ≥ 0.82 / RMSE ≤ 0.45.
- Research 0649 (`docs/research/0649-chug-hdr-wide-mos-feature-schema.md`).
- Held-out result: `.workingdir2/training/validation/chug_held_out_test_20260527.json`.
- Source: req — "Write a CHUG MOS-head held-out test validator script + run it + report results."
