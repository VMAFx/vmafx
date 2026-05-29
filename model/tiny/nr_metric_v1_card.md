# `nr_metric_v1` — model card

> Full operator-facing doc: [`docs/ai/models/nr_metric_v1.md`](../../docs/ai/models/nr_metric_v1.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `nr_metric_v1` |
| Files | `model/tiny/nr_metric_v1.onnx` + `nr_metric_v1.int8.onnx` (dynamic-PTQ INT8) |
| Sidecar | `model/tiny/nr_metric_v1.json` |
| Architecture | MobileNet-tiny — depthwise-separable Conv stack, ~19 K params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production baseline (C2 NR quality metric) |

## Training data + provenance

KoNViD-1k (1 200 clips, CC BY 4.0; not redistributed in-tree). Middle frame
per clip extracted at 224×224 grayscale. Crowd-sourced MOS labels (1–5 scale,
Amazon Mechanical Turk). Split: ~973 train / ~107 val / ~120 test (seed 42).

## Hyperparameters

MobileNet-tiny depthwise-separable Conv stack. Trained per
`ai/scripts/train_konvid.py`; exported via `ai/scripts/export_tiny_models.py`.
Dynamic-PTQ INT8 sidecar (ADR-0174); `quant_accuracy_budget_plcc = 0.01`.

## Eval metrics

| Metric | Value |
| --- | --- |
| Val MSE | ~0.382 |
| Val RMSE | ~0.62 MOS (on 1–5 scale) |

PLCC / SROCC against external KoNViD-1k test split not yet published in
this fork; the RMSE is from the training-validation split.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Input**: `input` — float32 NCHW `[1, 1, 224, 224]` grayscale luma, `[0, 1]`
- **Output**: `mos_score` — float32 `[1]`, approx MOS range `[1, 5]`
- **Resolution**: fixed 224×224 (caller must resize/crop upstream)
- **Bit depth**: 8 bpc (luma only)

## Known limits

- No-reference: content-blind (no reference stream used).
- Fixed resolution input; clip-level quality is obtained by averaging
  frame predictions.
- Trained on KoNViD-1k UGC only; may underperform on professionally produced
  SDR content or HDR sources.

## License + lineage

BSD-3-Clause-Plus-Patent. Trained on KoNViD-1k (CC BY 4.0, not redistributed).
See ADR-0168, ADR-0174, `registry.json` entry `nr_metric_v1`.

## See also

- [`docs/ai/models/nr_metric_v1.md`](../../docs/ai/models/nr_metric_v1.md) — full doc
- [`registry.json`](registry.json)
