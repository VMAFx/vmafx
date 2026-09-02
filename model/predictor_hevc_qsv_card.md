# `predictor_hevc_qsv` — VMAF predictor model card

- **Codec adapter**: `hevc_qsv`
- **Training date**: 2026-05-14
- **ONNX opset**: 18
- **Graph nodes**: 10
- **File**: `model/predictor_hevc_qsv.onnx` (21877 bytes)
- **SHA-256**: `3a157b816bb71b8fdc9fc42d1308e93880bf5d97d8d2f27253be1c55a138b609`


## 1. Purpose

Per-shot VMAF predictor for the `hevc_qsv` adapter. Consumed by
`vmaftune.predictor.Predictor` at runtime to pick the CRF that hits a
target VMAF without measuring VMAF on every shot. See
[`docs/ai/predictor.md`](../docs/ai/predictor.md) for the full
predict-then-verify loop.

## 2. Training data

- **Corpus kind**: `real-N=1620`
- **Train rows**: 1296
- **Held-out rows**: 324
- **Split**: 80 / 20 with seeded shuffle (seed = 42).
- **Schema**: vmaf-tune Phase A JSONL (`CORPUS_ROW_KEYS` v2).

## 3. Op allowlist compliance

Validated against `core/src/dnn/op_allowlist.c` via
`ai/src/vmaf_train/op_allowlist.py`:

- **Status**: OK

The graph uses only `Gemm`, `Relu`, `Sigmoid`, `Mul`, `Sub`, `Div`,
`Constant` — all on the libvmaf allowlist.

## 4. Validation metrics

Computed on the 20 % held-out split.

| Metric | Value |
|--------|-------|
| PLCC   | 0.8302 |
| SROCC  | 0.8322 |
| RMSE   | 9.7754 VMAF |

## 5. Signing

- **Sigstore signature**: unsigned in-tree artefact. Release automation attaches the Sigstore-keyless OIDC signature for the published tag; verify that bundle when consuming release assets. See
[`docs/development/release.md`](../docs/development/release.md).

## Architecture

Tiny MLP, 14 inputs × 64 hidden × 1 output:

```
input ────► (x − mean) / std ────► Gemm 14→64 ─► ReLU ─►
            Gemm 64→64 ─► ReLU ─► Gemm 64→1 ─► Sigmoid×100 ─► vmaf
```

Per-feature input normalisation is baked into the graph as
`Constant` buffers so ONNX Runtime CPU inference matches the
PyTorch trainer's behaviour bit-for-bit.

## Inputs

| Index | Name                          | Range          |
|-------|-------------------------------|----------------|
|   0   | `crf`                         | adapter range  |
|   1   | `probe_bitrate_kbps`          | ≥ 0            |
|   2   | `probe_i_frame_avg_bytes`     | ≥ 0            |
|   3   | `probe_p_frame_avg_bytes`     | ≥ 0            |
|   4   | `probe_b_frame_avg_bytes`     | ≥ 0            |
|   5   | `saliency_mean`               | 0..1           |
|   6   | `saliency_var`                | ≥ 0            |
|   7   | `frame_diff_mean`             | ≥ 0            |
|   8   | `y_avg`                       | ≥ 0            |
|   9   | `y_var`                       | ≥ 0            |
|  10   | `shot_length_frames`          | ≥ 1            |
|  11   | `fps`                         | > 0            |
|  12   | `width`                       | > 0            |
|  13   | `height`                      | > 0            |

## Output

`vmaf` — single scalar in `[0, 100]`.
