# `saliency_student_v1` — model card

> Full operator-facing doc: [`docs/ai/models/saliency_student_v1.md`](../../docs/ai/models/saliency_student_v1.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `saliency_student_v1` |
| File | `model/tiny/saliency_student_v1.onnx` |
| Sidecar | `model/tiny/saliency_student_v1.json` |
| Architecture | Tiny U-Net, 3-down + 3-up + skips, ~113 K params; `ConvTranspose2d` upsampler |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Retained for regression baselines; superseded by `saliency_student_v2` (ADR-0444) |

## Training data + provenance

DUTS-TR (Wang et al. 2017) — 10 553 images with binary fixation maps, 5 %
held-out validation fold under seed=42. Not redistributed in-tree; source:
<https://saliencydetection.net/duts/>.

## Hyperparameters

Encoder channels 16 → 32 → 48; bottleneck 48 ch. Loss: BCE + Dice. Adam
lr=1e-3, CosineAnnealingLR(T=50), 50 epochs, batch 32, crop 256×256, seed=42.

## Eval metrics

| Metric | Value |
| --- | --- |
| Best val IoU (5 % DUTS-TR fold) | 0.6558 |
| PyTorch ↔ ONNX max-abs-diff | 1.49e-6 |

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Input**: `input` — float32 NCHW `[1, 3, H, W]` ImageNet-normalised RGB
- **Output**: `saliency_map` — float32 `[1, 1, H, W]` per-pixel saliency `[0, 1]`
- **Resolution**: fully convolutional (trained at 256×256 crops)
- **Bit depth**: RGB float input

## Known limits

- `ConvTranspose2d` decoder; superseded by `saliency_student_v2` (bilinear
  resize + Conv, IoU 0.7105 vs 0.6558, +8.3%).
- No external DUTS-TE / ECSSD evaluation published in this fork yet.

## License + lineage

BSD-3-Clause-Plus-Patent. Trained on DUTS-TR (not redistributed). See
ADR-0286, `registry.json` entry `saliency_student_v1`.

## See also

- [`docs/ai/models/saliency_student_v1.md`](../../docs/ai/models/saliency_student_v1.md) — full doc
- [`saliency_student_v2_card.md`](saliency_student_v2_card.md) — production default
