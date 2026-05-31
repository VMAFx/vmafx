<!-- markdownlint-disable MD060 -->
# CHUG HDR MOS head — held-out test validator

`ai/scripts/validate_chug_hdr_mos_head.py` evaluates a trained CHUG HDR MOS
head ONNX against the 552-row held-out `test` partition of the CHUG corpus.
This partition was never used during training or model selection.

## Purpose

The CHUG corpus has explicit `train` / `val` / `test` splits in every feature
JSONL row. The trainer and seed-sweep use only `train` and `val`. The `test`
partition is kept strictly held-out so that the final production gate can be
evaluated without optimistic bias from hyperparameter tuning.

## Held-out production gate

| Metric | Threshold |
|--------|-----------|
| PLCC   | ≥ 0.85   |
| SROCC  | ≥ 0.82   |
| RMSE   | ≤ 0.45 MOS units |

All three must pass for exit 0. Exit 2 means the gate failed; exit 1 means
an input or inference error (not a gate verdict). Thresholds mirror the
ADR-0325 training gate and are never lowered on a miss.

## Usage

```bash
python ai/scripts/validate_chug_hdr_mos_head.py \
    --onnx .workingdir2/training/models/chug_hdr_mos_head_v1_wide_seed20260521.onnx \
    --out-json  .workingdir2/training/validation/chug_held_out_test_YYYYMMDD.json \
    --out-md    .workingdir2/training/validation/chug_held_out_test_YYYYMMDD.md
```

Default ONNX path is
`.workingdir2/training/models/chug_hdr_mos_head_v1_wide_seed20260521.onnx`.
Override via `VMAF_CHUG_HDR_ONNX` environment variable.

Default shard directory is
`.corpus/chug/training/fr_canonical_shards/output/`.
Override via `--shard-dir` or pass explicit `--feature-jsonl` paths.

## Arguments

| Flag | Default | Description |
|------|---------|-------------|
| `--onnx` | see above | CHUG MOS head ONNX path |
| `--shard-dir` | `.corpus/chug/…/output` | Dir searched for `shard_*.features.jsonl` |
| `--feature-jsonl` | (from shard-dir) | Explicit shard path; may be repeated |
| `--out-json` | `.workingdir2/training/validation/…json` | JSON report + run-manifest |
| `--out-md` | `.workingdir2/training/validation/…md` | Markdown summary |
| `--gate-plcc` | 0.85 | Override PLCC threshold (for testing) |
| `--gate-srocc` | 0.82 | Override SROCC threshold |
| `--gate-rmse` | 0.45 | Override RMSE threshold |

## Outputs

The validator emits three files:

1. **JSON report** (`--out-json`): full metrics, per-field gate verdict,
   first-5-row sample predictions/MOS, and an `ai-run-provenance-v1` sidecar
   block recording the ONNX path, shard paths, argv, and git HEAD.
2. **Markdown report** (`--out-md`): human-readable table for PR descriptions.
3. **Console summary**: PLCC / SROCC / RMSE and PASS / FAIL verdict.

## Current result (2026-05-27)

Model `chug_hdr_mos_head_v1_wide_seed20260521.onnx`:

| Metric | Val | Test (held-out) | Gate |
|--------|-----|-----------------|------|
| PLCC   | 0.8733 | **0.8468** | FAIL (< 0.85) |
| SROCC  | 0.8528 | **0.8188** | FAIL (< 0.82) |
| RMSE   | 0.2512 | **0.2639** | OK   |

Model status: **Proposed** (not promoted). See
`docs/research/0715-chug-hdr-held-out-test-validation-2026-05-27.md`
for interpretation and next steps.

## Feature schema

The validator uses the `chug-hdr-wide-v1` 34-column schema, identical to the
training path in `train_chug_hdr_mos_head.py`. It reuses `_row_to_features`
and `_normalise_split` from `train_konvid_mos_head.py` to ensure the
feature projection is byte-identical to the training path.

## Dependencies

- `onnxruntime` (CPU provider)
- `numpy`
- `aiutils.cli_helpers`, `aiutils.run_manifest` (in-repo helpers)
