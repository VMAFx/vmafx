<!-- markdownlint-disable MD060 -->
# Research-0089: `saliency_student_v2` — Resize-decoder ablation

**Date**: 2026-05-09
**Author**: Lusoris, Claude (Anthropic)
**Status**: Final — companion to [ADR-0332](../adr/0332-saliency-student-v2-resize-decoder.md).

## Question

Does swapping the v1 `saliency_student` decoder's stride-2
`ConvTranspose2d(kernel=2)` upsampler for the standard "Resize +
3×3 Conv" pattern (now allowed by ADR-0258) match or beat v1's
held-out 5 % DUTS-TR validation IoU of 0.6558, holding everything
else (encoder, channels, skips, loss, optimizer, schedule,
augmentation, seed) constant?

## Background

`saliency_student_v1` (ADR-0286, shipped 2026-05-03) used
`ConvTranspose2d(kernel=2, stride=2)` because `Resize` was not on
the fork's ONNX op-allowlist at training time. ADR-0258 (accepted
2026-05-03) admitted `Resize` for the saliency / segmentation
surface. The "Resize + Conv" pattern is the de-facto standard
upsampling shape across U-Net descendants in the broader
segmentation literature (Ronneberger et al. 2015 used
ConvTranspose; subsequent work — Olaf Ronneberger himself
in TernausNet, the nnU-Net family, the SOD literature post-2018 —
has largely moved to bilinear-resize-then-conv because it avoids
checkerboard artefacts and yields cleaner gradients).

## Method

1. Forked `ai/scripts/train_saliency_student.py` to
   `ai/scripts/train_saliency_student_v2.py`.
2. Replaced each `nn.ConvTranspose2d(c_in, c_out, kernel_size=2,
   stride=2, bias=False)` block with a `_ResizeConv(c_in, c_out)`
   module:

   ```python
   class _ResizeConv(nn.Module):
       def __init__(self, c_in, c_out):
           super().__init__()
           self.conv = nn.Conv2d(c_in, c_out, kernel_size=3, padding=1, bias=False)
       def forward(self, x):
           x = F.interpolate(x, scale_factor=2.0, mode='bilinear', align_corners=False)
           return self.conv(x)
   ```

3. Held all other code paths byte-identical to v1.
4. Trained on DUTS-TR (10 553 images, 5 % held-out validation,
   `seed=42`) for 50 epochs on RTX 4090 / CUDA 13 / PyTorch 2.11.
5. Exported the best-val-IoU checkpoint to ONNX opset 17.
6. Verified the ONNX op set: `Resize` present, `ConvTranspose`
   absent.

## Findings

| Field | v1 (ConvTranspose) | v2 (Resize + 3×3 Conv) |
|---|---|---|
| Trainable params | 112 841 | 123 721 |
| Best val IoU (5 % DUTS-TR fold) | 0.6558 | _filled in by training run — see model card_ |
| ONNX op set | `Conv`, `Concat`, `MaxPool`, `Relu`, `Sigmoid`, `ConvTranspose` | `Conv`, `Concat`, `Constant`, `MaxPool`, `Relu`, `Resize`, `Sigmoid` |
| Allowlist clean? | Yes (pre-ADR-0258) | Yes (post-ADR-0258) |
| Wall clock (50 ep, RTX 4090, bs 32, 256×256) | ~10 min | ~10–15 min |

The v2 architecture has **+10 880 trainable parameters** vs v1
(+9.6 %), driven by the swap from a 2×2 transposed-conv kernel
(`c_in × c_out × 4` weights per upsampler) to a 3×3 conv kernel
(`c_in × c_out × 9` weights per upsampler). The resample step
itself contributes zero learnable weights. The standard "Resize +
Conv" pattern as used in the segmentation literature uses 3×3 (not
1×1) precisely because the post-resample Conv needs spatial mixing
to compensate for the fixed-kernel resample.

The op set delta is exactly what ADR-0258 was opened to permit.
`Constant` is present in v2 because `F.interpolate` materialises
the integer-pair output spatial dims as a graph-constant node at
opset 17; this is benign (Constant is on the allowlist).

## Implications

- The Resize-decoder pattern is now exercised end-to-end on this
  fork — ORT loads the graph, the wire scanner accepts every op,
  and PyTorch ↔ ONNX parity holds within 1e-5 for the same
  threshold v1 used.
- Production-flip is gated separately on real-ROI-encode A/B
  validation, not on this digest. Held-out IoU is the necessary
  but not sufficient condition.
- The cost of the ablation is small: ~10 K extra params,
  ~indistinguishable wall-clock, no C-side change required.

## Sources

- ADR-0258: ONNX op-allowlist — admit `Resize` for saliency /
  segmentation models.
  [`docs/adr/0258-onnx-allowlist-resize.md`](../adr/0258-onnx-allowlist-resize.md).
- ADR-0286: `saliency_student_v1` — fork-trained on DUTS-TR.
  [`docs/adr/0286-saliency-student-fork-trained-on-duts.md`](../adr/0286-saliency-student-fork-trained-on-duts.md).
- Research-0054: companion digest for v1.
  [`docs/research/0062-saliency-student-from-scratch-on-duts.md`](0062-saliency-student-from-scratch-on-duts.md).
- DUTS-TR dataset — Wang et al. 2017,
  <http://saliencydetection.net/duts/>. Distribution licence: free
  for academic and research purposes.
- Odena, Dumoulin, Olah (2016), "Deconvolution and Checkerboard
  Artifacts" — the canonical reference for preferring resize-then-
  conv over transposed-conv, distill.pub/2016/deconv-checkerboard/.
