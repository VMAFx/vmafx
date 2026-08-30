- **Tiny-AI / saliency**: Added `saliency_student_v1` — a fork-trained
  tiny U-Net (~113 K parameters, ONNX opset 17, BSD-3-Clause-Plus-Patent)
  trained from scratch on the DUTS-TR public saliency-detection corpus
  (Wang et al. 2017). Replaces the smoke-only
  `mobilesal_placeholder_v0` as the recommended weights for the
  `mobilesal` feature extractor. The C-side `feature_mobilesal.c`
  extractor is unchanged (same `input` / `saliency_map` tensor names,
  same NCHW shapes); the new model is a true drop-in. The decoder uses
  `ConvTranspose` for stride-2 upsampling so every op in the graph is
  on `core/src/dnn/op_allowlist.c` without an allowlist patch in
  the same PR. DUTS images are not redistributed in-tree; only the
  trained weights are. The placeholder remains in the registry with
  `smoke: true` for legacy reasons. New model card at
  [`docs/ai/models/saliency_student_v1.md`](docs/ai/models/saliency_student_v1.md);
  decision in
  [ADR-0286](docs/adr/0286-saliency-student-fork-trained-on-duts.md);
  digest in
  [Research-0054](docs/research/0062-saliency-student-from-scratch-on-duts.md).
  Trainer at `ai/scripts/train_saliency_student.py`.
