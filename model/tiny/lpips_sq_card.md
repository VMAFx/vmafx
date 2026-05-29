# `lpips_sq_v1` — model card (LPIPS-SqueezeNet)

> Full operator-facing doc: [`docs/ai/models/lpips_sq.md`](../../docs/ai/models/lpips_sq.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `lpips_sq_v1` |
| File | `model/tiny/lpips_sq.onnx` |
| Sidecar | `model/tiny/lpips_sq.json` |
| Architecture | SqueezeNet-based LPIPS (richzhang/PerceptualSimilarity v0.1) |
| ONNX opset | 18 |
| License | BSD-2-Clause (upstream richzhang/PerceptualSimilarity) |
| Status | Production; upstream-weights wrapped for fork I/O contract |

## Training data + provenance

Upstream pretrained weights from richzhang/PerceptualSimilarity v0.1
(Zhang et al. 2018, "The Unreasonable Effectiveness of Deep Features as a
Perceptual Metric"). Trained on the BAPPS dataset. Exported via
`ai/lpips_export.py`. Upstream pinned at
`github.com/richzhang/PerceptualSimilarity`.

## Hyperparameters

SqueezeNet backbone with LPIPS linear calibration layers. Trained on BAPPS
(perceptual similarity 2AFC judgements). Architecture fixed by upstream.

## Eval metrics

Refer to the upstream CVPR 2018 paper for BAPPS 2AFC scores. Fork-specific
PLCC / SROCC against VMAF/MOS holdouts not yet published.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Inputs**: `ref` `[1, 3, H, W]` + `dist` `[1, 3, H, W]` — ImageNet-normalised RGB NCHW
- **Output**: `score` — float32 scalar perceptual distance (lower = more similar)
- **Resolution**: any (fully convolutional in SqueezeNet)
- **Bit depth**: input must be float in `[-1, 1]` (ImageNet normalisation)

## Known limits

- Trained on 2AFC perceptual similarity (patch-level); not directly calibrated
  to VMAF MOS scale.
- RGB input only — chroma subsampling / YUV content must be converted.
- Upstream BSD-2-Clause license applies to the weights.

## License + lineage

BSD-2-Clause (upstream). Fork wrapper code BSD-3-Clause-Plus-Patent.
`registry.json` entry `lpips_sq_v1`.

## See also

- [`docs/ai/models/lpips_sq.md`](../../docs/ai/models/lpips_sq.md) — full doc
- [`registry.json`](registry.json)
