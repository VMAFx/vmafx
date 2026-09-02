- **Tiny-AI training scaffold for the Netflix VMAF corpus (ADR-0242).**
  Prepares the fork's tiny-AI training workstream to train on the local
  Netflix VMAF corpus (9 reference YUVs + 70 distorted YUVs at
  `.workingdir2/netflix/`, gitignored, never committed). The scaffold
  defines: the `--data-root` loader API, the `NflxLocalDataset` class
  in `ai/data/`, the `vmaf_v0.6.1` distillation-vs-from-scratch policy
  decision table, and the model-size alternatives space (micro / small /
  medium MLP). No training runs; no Netflix golden assertions touched.
  Deliverables: [ADR-0242](docs/adr/0242-tiny-ai-netflix-training-corpus.md)
  (architecture decision + alternatives table), [Research-0019](docs/research/0019-tiny-ai-netflix-training.md)
  (VMAF methodology survey + distillation literature), MCP end-to-end
  smoke test at `mcp-server/vmaf-mcp/tests/test_smoke_e2e.py` (exercises
  `vmaf_score` against the Netflix golden fixture — one-command MCP
  health check), and `docs/ai/training-data.md` (corpus path convention,
  loader API, evaluation harness). Actual training deferred to a
  follow-up PR pending architecture selection.
