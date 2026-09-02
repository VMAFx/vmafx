- **Tiny-AI Netflix corpus training scaffold — draft PR registration (ADR-0417).**
  Opens `ai/tiny-netflix-training-scaffold` as a formal draft PR so the user can
  review and confirm architecture choices before triggering a training run against
  the local Netflix VMAF corpus (`.workingdir2/netflix/`, gitignored, 37 GB). This
  PR adds no new code; it bundles the scaffold deliverables already in `master`
  (ADR-0242, Research Digest 0019, `docs/ai/training-data.md`,
  `mcp-server/vmaf-mcp/tests/test_smoke_e2e.py`) for a single-URL review point.
- **Research Digest 0099** (`docs/research/0099-tiny-ai-netflix-training-update.md`):
  2024–2026 literature update covering knowledge-distillation techniques for perceptual
  quality metrics (temperature-scaled distillation, corpus-size regime analysis,
  ensemble teacher distillation), ONNX Runtime 1.18–1.21 changes relevant to the
  fork's inference path (opset 19/21 additions, CoreML EP support), lightweight FR
  regressor architecture findings (wide-shallow vs narrow-deep at n = 70, learned
  feature-reweighting, VMAF-Lite 2-feature subset), and MCP 2025-03-26 structural
  tool result alignment. Extends Digest 0019; informs ADR-0417.
