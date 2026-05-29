# `learned_filter_v1` — model card

> Full operator-facing doc: [`docs/ai/models/learned_filter_v1.md`](../../docs/ai/models/learned_filter_v1.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `learned_filter_v1` |
| Files | `model/tiny/learned_filter_v1.onnx` + `learned_filter_v1.int8.onnx` |
| Sidecar | `model/tiny/learned_filter_v1.json` |
| Architecture | Tiny residual filter baseline — ~19 K params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production baseline for `vmaf_pre` pre-filter path |

## Training data + provenance

Self-supervised on KoNViD-1k frames (CC BY 4.0; not redistributed) with
synthetic blur + JPEG degradation. Baseline for the `vmaf_pre` learned
pre-filter capability (ADR-0168).

## Hyperparameters

~19 K params. Dynamic-PTQ INT8 sidecar via `ai/scripts/ptq_dynamic.py`;
`quant_accuracy_budget_plcc = 0.01`.

## Eval metrics

Self-supervised restoration quality; no external MOS holdout published in
this fork. The filter's downstream impact is measured via VMAF delta on
KoNViD-1k frames.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Input**: degraded luma frame `[1, 1, H, W]` float32
- **Output**: restored luma `[1, 1, H, W]` float32
- **Resolution**: fully convolutional (any H×W)
- **Bit depth**: 8 bpc luma

## Known limits

- Self-supervised on synthetic degradation; may not generalise to all
  real-world compression artefact types.
- Luma-only; no chroma filtering.

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0168, `registry.json` entry `learned_filter_v1`.

## See also

- [`docs/ai/models/learned_filter_v1.md`](../../docs/ai/models/learned_filter_v1.md) — full doc
- [`registry.json`](registry.json)
