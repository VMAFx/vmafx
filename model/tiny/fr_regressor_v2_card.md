# `fr_regressor_v2` — model card

> Full operator-facing doc: [`docs/ai/models/fr_regressor_v2.md`](../../docs/ai/models/fr_regressor_v2.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `fr_regressor_v2` |
| File | `model/tiny/fr_regressor_v2.onnx` |
| Sidecar | `model/tiny/fr_regressor_v2.json` |
| Architecture | Tiny MLP — 6 canonical features + 8-D codec block → 14 inputs, hidden 64 |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production checkpoint (codec-aware, ENCODER_VOCAB v2) |

## Training data + provenance

vmaf-tune Phase A JSONL corpus (ADR-0237), 216 rows; 80/20 split (seed=42).
Teacher label: `vmaf_v0.6.1` per-frame score. Codec-aware: 6 encoder
one-hots (libx264, libx265, libsvtav1, libvvenc, libvpx-vp9, h264/hevc/av1
nvenc/qsv) + `preset_norm` + `crf_norm`.

## Hyperparameters

14 inputs (6 canonical + 8 codec), hidden 64, 1 output. See
`ai/scripts/train_fr_regressor_v2.py` for the full recipe.

## Eval metrics

| Metric | Value |
| --- | --- |
| In-sample PLCC | 0.9794 |
| In-sample SROCC | 0.9640 |
| In-sample RMSE | 3.014 VMAF |

In-sample only (216 rows; small corpus). Use `fr_regressor_v3` (ENCODER_VOCAB
v3, 5640 rows) for the larger corpus evaluation.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Inputs**: `features` `[N, 6]` + `codec` `[N, 8]` (ENCODER_VOCAB v2 one-hot)
- **Output**: `score` `[N]` float32, VMAF scale

## Known limits

- ENCODER_VOCAB v2 (12 slots); superseded by v3 (16 slots with AMF + Apple VT).
- Small training corpus (216 rows). Superseded by `fr_regressor_v3`.

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0272, ADR-0235, `registry.json` entry `fr_regressor_v2`.

## See also

- [`docs/ai/models/fr_regressor_v2.md`](../../docs/ai/models/fr_regressor_v2.md)
- [`docs/ai/models/fr_regressor_v3.md`](../../docs/ai/models/fr_regressor_v3.md) — successor
- [`registry.json`](registry.json)
