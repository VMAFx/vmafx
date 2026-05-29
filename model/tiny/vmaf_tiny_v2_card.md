# `vmaf_tiny_v2` — model card

> Full operator-facing doc: [`docs/ai/models/vmaf_tiny_v2.md`](../../docs/ai/models/vmaf_tiny_v2.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v2` |
| File | `model/tiny/vmaf_tiny_v2.onnx` |
| Sidecar | `model/tiny/vmaf_tiny_v2.json` |
| Architecture | `mlp_small` — Linear(6,16) → ReLU → Linear(16,8) → ReLU → Linear(8,1), ~257 params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production default tiny FR fusion model |

## Training data + provenance

4-corpus parquet (`runs/full_features_4corpus.parquet`, 330 499 rows):
Netflix Public Dataset (9 sources), KoNViD-1k (5-fold, CC BY 4.0),
BVI-DVC subsets A–D. Teacher label: `vmaf_v0.6.1` per-frame score.

## Hyperparameters

90 epochs, Adam lr=1e-3, MSE loss, batch 256. StandardScaler (mean/std)
baked into the ONNX graph as Constant nodes.

## Eval metrics

| Split | PLCC | SROCC |
| --- | --- | --- |
| Netflix 9-fold LOSO (mean ± std) | 0.9978 ± 0.0021 | — |
| KoNViD 5-fold (mean) | 0.9998 | — |
| Ship gate (in-sample) | 0.9999 | — |

## Operating point

- **Backend**: CPU / CUDA / SYCL / OpenVINO / ROCm (ONNX Runtime EP)
- **Resolution**: any (feature-based input)
- **Bit depth**: 8 bpc and 10 bpc (features are bit-depth agnostic)
- **Input**: `features` — float32 `[N, 6]`: `adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2`
- **Output**: `vmaf` — float32 `[N]`, range `[0, 100]`

## Known limits

- Codec-agnostic (does not use encoder type / CRF). Use `fr_regressor_v2` or
  `fr_regressor_v3` for codec-conditioned predictions.
- No HDR support — the teacher label is the SDR-trained `vmaf_v0.6.1`.
- Feature-based: pixel resolution affects feature extraction upstream, not
  this model.

## License + lineage

BSD-3-Clause-Plus-Patent. Derived from Netflix Public Dataset (local extract,
not redistributed) + KoNViD-1k (CC BY 4.0, not redistributed). See
`registry.json` entry `vmaf_tiny_v2`.

## See also

- [`docs/ai/models/vmaf_tiny_v2.md`](../../docs/ai/models/vmaf_tiny_v2.md) — full doc
- [`registry.json`](registry.json) — registry entry with SHA-256 + Sigstore bundle
