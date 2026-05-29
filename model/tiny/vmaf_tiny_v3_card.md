# `vmaf_tiny_v3` — model card

> Full operator-facing doc: [`docs/ai/models/vmaf_tiny_v3.md`](../../docs/ai/models/vmaf_tiny_v3.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v3` |
| Files | `model/tiny/vmaf_tiny_v3.onnx` + `vmaf_tiny_v3.int8.onnx` (dynamic-PTQ INT8) |
| Sidecar | `model/tiny/vmaf_tiny_v3.json` |
| Architecture | `mlp_medium` — Linear(6,32) → ReLU → Linear(32,16) → ReLU → Linear(16,1), ~769 params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production shipped; opt-in higher-tier over `vmaf_tiny_v2` |

## Training data + provenance

4-corpus parquet (`runs/full_features_4corpus.parquet`, 330 499 rows):
Netflix Public Dataset (9 sources), KoNViD-1k (CC BY 4.0), BVI-DVC A–D.
Teacher label: `vmaf_v0.6.1` per-frame score. Same corpus as v2.

## Hyperparameters

90 epochs, Adam lr=1e-3, MSE loss, batch 256. StandardScaler baked into
ONNX graph. Dynamic-PTQ INT8 sidecar via `ai/scripts/ptq_dynamic.py`
(ADR-0275); measured PLCC drop = 0.000120 vs 0.01 budget.

## Eval metrics

| Split | PLCC | SROCC |
| --- | --- | --- |
| Netflix 9-fold LOSO (mean ± std) | 0.9986 ± 0.0015 | — |

## Operating point

- **Backend**: CPU / CUDA / SYCL / OpenVINO / ROCm (ONNX Runtime EP)
- **Resolution**: any (feature-based)
- **Bit depth**: 8 bpc and 10 bpc
- **Input**: `features` — float32 `[N, 6]`
- **Output**: `vmaf` — float32 `[N]`, range `[0, 100]`
- **INT8 sidecar**: use `vmaf_tiny_v3.int8.onnx` for quantised inference
  (PLCC budget guaranteed within 0.01 of fp32)

## Known limits

- Same codec-blind limits as v2. Use v3 only when you need the step up in
  PLCC headroom over v2 (0.9986 vs 0.9978) and don't need codec conditioning.
- Architecture ladder is saturated at v4 — v3 and v4 are statistically
  indistinguishable (same PLCC ± std). Pick v3 for smaller ONNX bytes.

## License + lineage

BSD-3-Clause-Plus-Patent. See `registry.json` entry `vmaf_tiny_v3`.

## See also

- [`docs/ai/models/vmaf_tiny_v3.md`](../../docs/ai/models/vmaf_tiny_v3.md) — full doc
- [`registry.json`](registry.json) — registry entry with SHA-256 + Sigstore bundle
