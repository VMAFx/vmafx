# `saliency_student_v2` — model card

> **One-screen summary** — the long-form operator-facing doc lives at
> [`docs/ai/models/saliency_student_v2.md`](../../docs/ai/models/saliency_student_v2.md).

## Identity

| Field | Value |
| --- | --- |
| Model id | `saliency_student_v2` |
| Lineage | Fork (`saliency_student_v2.onnx`); architectural successor to `saliency_student_v1` (the production weights for the C-side `mobilesal` extractor); upstream lineage = none |
| Files | `model/tiny/saliency_student_v2.onnx`, `model/tiny/saliency_student_v2.json` |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent (weights wholly fork-owned) |
| Status | Parallel artefact under `model/tiny/`. Production weights for the `mobilesal` extractor remain `saliency_student_v1` until a follow-up PR validates v2 in real ROI encodes. |

## Intended use

Same as v1: drop-in replacement weights for `feature_mobilesal.c`,
producing a per-frame `saliency_mean` content descriptor in `[0, 1]`.
v2 is the architectural ablation that swaps v1's `ConvTranspose`
upsampler for the standard "Resize + Conv" pattern enabled by
[ADR-0258](../../docs/adr/0258-onnx-allowlist-resize.md).

## Inputs / outputs

| Tensor | Type | Shape | Notes |
| --- | --- | --- | --- |
| `input` | float32 | `[1, 3, H, W]` | ImageNet-normalised RGB, NCHW. Same as v1. |
| `saliency_map` | float32 | `[1, 1, H, W]` | per-pixel saliency in `[0, 1]`. Same as v1. |

`H`, `W` are dynamic — fully convolutional. Trained at 256×256
random crops.

## Training (recipe identical to v1 except for the decoder)

| Field | v1 | v2 |
| --- | --- | --- |
| Backbone | TinyU-Net 3-down + 3-up + skips | TinyU-Net 3-down + 3-up + skips |
| Encoder channels | 16 → 32 → 48 | 16 → 32 → 48 |
| Bottleneck | 48 ch | 48 ch |
| Decoder upsampler | `ConvTranspose2d(k=2, s=2, no bias)` | `F.interpolate(scale=2, bilinear, align_corners=False)` + `Conv2d(k=3, p=1, no bias)` |
| Loss | BCE + Dice | BCE + Dice |
| Optimizer | Adam, lr=1e-3 | Adam, lr=1e-3 |
| Schedule | CosineAnnealingLR(T=50) | CosineAnnealingLR(T=50) |
| Epochs | 50 | 50 |
| Batch size | 32 | 32 |
| Crop | 256 | 256 |
| Seed | 42 | 42 |
| Trainable params | 112 841 | 123 721 |
| Corpus | DUTS-TR (10 553 images, 5 % held-out validation fold) | DUTS-TR (same 10 553 images, same 5 % fold under seed=42) |
| Best val IoU (5 % DUTS-TR fold) | 0.6558 | **0.7105** (gate PASS; +0.0547 / +8.3 % vs v1) |
| ONNX export | opset 17, do_constant_folding=True, eval mode | opset 17, do_constant_folding=True, eval mode |
| PyTorch ↔ ONNX parity max-abs-diff | 1.49e-6 | 3.278e-6 (threshold 1e-5) |

The training script (`ai/scripts/train_saliency_student_v2.py`) is
deterministic given the seed and pinned PyTorch / CUDA versions.

## Op-allowlist conformance

ONNX op set:

| Op | v1 | v2 |
| --- | --- | --- |
| `Conv` | yes | yes |
| `Concat` | yes | yes |
| `Constant` | no | yes (graph-constant for resample target dims; benign, on allowlist) |
| `MaxPool` | yes | yes |
| `Relu` | yes | yes |
| `Sigmoid` | yes | yes |
| `ConvTranspose` | yes | **no** |
| `Resize` | no | **yes** (`mode='linear'`, `coordinate_transformation_mode='half_pixel'`) |

Every op is on
[`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c)
post-[ADR-0258](../../docs/adr/0258-onnx-allowlist-resize.md).
ADR-0258 admits `Resize` op-type-only; attribute enforcement is
delegated to ORT, which accepts the bilinear / half-pixel
combination unconditionally.

## Eval evidence

- 5 % DUTS-TR validation fold IoU (in-loop, deterministic under
  `seed=42`): see `build_artifacts/saliency_student_v2_train.json`.
- PyTorch ↔ ONNX parity max-abs-diff threshold: 1e-5 (same as v1).
  Hard-fail in the trainer if exceeded.
- External (DUTS-TE / ECSSD) evaluation is a follow-up shared with
  v1, per ADR-0286 backlog.

## Reproducing

```bash
mkdir -p $HOME/datasets/duts && cd $HOME/datasets/duts
wget https://saliencydetection.net/duts/download/DUTS-TR.zip
unzip DUTS-TR.zip

cd /path/to/vmaf
.venv/bin/python ai/scripts/train_saliency_student_v2.py \
    --duts-root $HOME/datasets/duts/DUTS-TR \
    --output    model/tiny/saliency_student_v2.onnx \
    --epochs 50 --batch-size 32 --lr 1e-3 --seed 42 \
    --metrics-out build_artifacts/saliency_student_v2_train.json

.venv/bin/python ai/scripts/validate_model_registry.py
```

## Cross-references

- [ADR-0258](../../docs/adr/0258-onnx-allowlist-resize.md) — admits
  `Resize` to the allowlist.
- [ADR-0286](../../docs/adr/0286-saliency-student-fork-trained-on-duts.md)
  — v1 decision record.
- [ADR-0332](../../docs/adr/0332-saliency-student-v2-resize-decoder.md)
  — v2 decision record (this PR).
- [Research-0089](../../docs/research/0089-saliency-student-v2-resize-decoder.md)
  — companion digest.
- [`docs/ai/models/saliency_student_v2.md`](../../docs/ai/models/saliency_student_v2.md)
  — operator-facing doc (ADR-0042 5-point bar).
- [`docs/ai/models/saliency_student_v1.md`](../../docs/ai/models/saliency_student_v1.md)
  — v1 operator-facing doc.
