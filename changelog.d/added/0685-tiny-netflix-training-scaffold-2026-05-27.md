- **Tiny-AI Netflix corpus training scaffold — 2026-05-27 prep scope (ADR-0685).**
  Re-opens the canonical `ai/tiny-netflix-training-scaffold` draft PR after the branch
  was absent from origin following PR #152 (`fix/volk-static-archive-priv-remap`,
  ADR-0198) merging. Consolidates prep work for training tiny-AI full-reference
  regressors on the local Netflix VMAF corpus (9 reference YUVs + 70 distorted YUVs
  at `.workingdir2/netflix/`, gitignored). Adds Research Digest 0730 surveying
  2026-05-22–27 developments: distillation convergence stability on small corpora,
  ONNX Runtime 1.21 INT8 MatMul improvements, and small-corpus architecture evidence
  confirming the 3×128 tiny MLP as the recommended starting point. Architecture
  selection and the first training run remain deferred to a follow-up PR; the 3×128
  MLP with temperature-scaled KL distillation (T ≈ 2–4) from `vmaf_v0.6.1` is the
  recommended starting point per Research-0730. No training executed; no Netflix golden
  assertions modified. References: [ADR-0685](docs/adr/0685-tiny-netflix-training-scaffold-2026-05-27.md),
  [ADR-0242](docs/adr/0242-tiny-ai-netflix-training-corpus.md),
  [Research-0730](docs/research/0730-tiny-ai-netflix-training-prep-2026-05-27.md).
