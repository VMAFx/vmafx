- **`saliency_student_v2` — Resize-decoder ablation
  ([ADR-0332](../docs/adr/0332-saliency-student-v2-resize-decoder.md),
  [Research-0089](../docs/research/0089-saliency-student-v2-resize-decoder.md)).**
  Fork-trained tiny U-Net (~124 K params) on DUTS-TR that swaps
  v1's `ConvTranspose` stride-2 upsampler for the standard "Resize +
  3×3 Conv" pattern enabled by
  [ADR-0258](../docs/adr/0258-onnx-allowlist-resize.md). Encoder /
  channels / loss / optimizer / schedule / seed are held identical
  to [`saliency_student_v1`](../docs/ai/models/saliency_student_v1.md)
  so the ablation is clean. Ships as a parallel artefact alongside
  v1 under `model/tiny/saliency_student_v2.onnx`; v1 remains the
  production weights for the C-side `mobilesal` extractor until a
  follow-up PR validates v2 in real ROI encodes. ONNX op set adds
  `Resize` (mode=`linear`, `coordinate_transformation_mode=half_pixel`)
  and drops `ConvTranspose`. New trainer
  `ai/scripts/train_saliency_student_v2.py`. User docs:
  [`docs/ai/models/saliency_student_v2.md`](../docs/ai/models/saliency_student_v2.md).
