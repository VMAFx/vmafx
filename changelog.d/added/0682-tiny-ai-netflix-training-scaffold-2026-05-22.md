- **Tiny-AI Netflix corpus training scaffold — 2026-05-22 prep scope (ADR-0682).**
  Opens the canonical `ai/tiny-netflix-training-scaffold` draft PR, consolidating
  the prep work for training tiny-AI full-reference regressors on the local Netflix
  VMAF corpus (9 reference YUVs + 70 distorted YUVs at `.workingdir2/netflix/`,
  gitignored). Adds Research Digest 0706 surveying 2025–2026 lightweight FR metric
  training literature (EfficientVMAF CVPR 2024, temperature-scaled distillation
  NeurIPS 2024, ONNX Runtime 1.20 MatMul improvements, learned feature reweighting
  ICASSP 2025). Architecture selection and the first training run remain deferred to
  a follow-up PR; the 3×128 tiny MLP with LOSO cross-validation is the recommended
  starting point per Research-0706. No training executed; no Netflix golden assertions
  modified. References: [ADR-0682](docs/adr/0682-tiny-ai-netflix-training-scaffold-2026-05-22.md),
  [ADR-0242](docs/adr/0242-tiny-ai-netflix-training-corpus.md),
  [Research-0706](docs/research/0706-tiny-ai-netflix-training-prep-2026-05-22.md).
