# `vmaf_tiny_v4` — model card

> Full operator-facing doc: [`docs/ai/models/vmaf_tiny_v4.md`](../../docs/ai/models/vmaf_tiny_v4.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v4` |
| Files | `model/tiny/vmaf_tiny_v4.onnx` + `vmaf_tiny_v4.int8.onnx` (dynamic-PTQ INT8) |
| Sidecar | `model/tiny/vmaf_tiny_v4.json` |
| Architecture | `mlp_large` — Linear(6,64) → ReLU → Linear(64,32) → ReLU → Linear(32,16) → ReLU → Linear(16,1), ~3073 params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Opt-in only; top-of-ladder; architecture ladder saturated here |

## Training data + provenance

4-corpus parquet (`runs/full_features_4corpus.parquet`, 330 499 rows):
Netflix Public Dataset (9 sources), KoNViD-1k (CC BY 4.0), BVI-DVC A–D.
Teacher label: `vmaf_v0.6.1` per-frame score. Same corpus as v2/v3.

## Hyperparameters

90 epochs, Adam lr=1e-3, MSE loss, batch 256. StandardScaler baked into
ONNX graph. Dynamic-PTQ INT8 sidecar (ADR-0275); measured PLCC
drop = 0.000145 vs 0.01 budget.

## Eval metrics

| Split | PLCC | SROCC |
| --- | --- | --- |
| Netflix 9-fold LOSO (mean ± std) | 0.9987 ± 0.0015 | — |

## Operating point

- **Backend**: CPU / CUDA / SYCL / OpenVINO / ROCm (ONNX Runtime EP)
- **Resolution**: any (feature-based)
- **Bit depth**: 8 bpc and 10 bpc
- **Input**: `features` — float32 `[N, 6]`
- **Output**: `vmaf` — float32 `[N]`, range `[0, 100]`

## Known limits

- Architecture ladder saturates at v4 — PLCC 0.9987 is statistically
  indistinguishable from v3 (0.9986); pick v3 for smaller ONNX bytes unless
  the absolute top of the measured ladder is required.
- Same codec-blind limits as v2/v3.

## License + lineage

BSD-3-Clause-Plus-Patent. See `registry.json` entry `vmaf_tiny_v4`.
ADR-0242 (`docs/adr/0242-vmaf-tiny-v4-mlp-large.md`) documents the
architecture decision and the "ladder saturates" verdict.

## See also

- [`docs/ai/models/vmaf_tiny_v4.md`](../../docs/ai/models/vmaf_tiny_v4.md) — full doc
- [`registry.json`](registry.json) — registry entry with SHA-256 + Sigstore bundle
