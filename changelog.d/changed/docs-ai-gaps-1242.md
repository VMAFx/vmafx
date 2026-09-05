**Close the three `docs/ai/` gaps of epic #1242** — factual state, not promises

- [`docs/ai/sidecar-online-training.md`](../../docs/ai/sidecar-online-training.md):
  the one-line "planned per Research-0733 §3.4" is replaced by a
  **Checkpoint quarantine (not implemented)** section that tabulates §3.4
  element by element against the tree. Atomic checkpoint writes and the
  `.sha256` sidecar exist; the stability gate, the fixture set, the
  `unstable` tag, `spec.versionPolicy`, `stability_plcc_delta`, automatic
  rollback, and node-side digest verification do not. Every committed
  checkpoint is picked up unvetted.
- [`docs/ai/extractor-template.md`](../../docs/ai/extractor-template.md):
  the large-N recipe row called `feature_transnet_v2.c` "planned". The
  extractor shipped as `core/src/feature/transnet_v2.c`; the new
  **Large sliding windows** subsection documents the real 100-slot ring
  buffer, the `[1, 100, 3, 27, 48]` tensor, the fact that the suggested
  decimation was *not* taken (the network runs once per frame), the
  ~50-frame warm-up, and that per-shot aggregation is not in the tree.
- [`docs/ai/inference.md`](../../docs/ai/inference.md): "planned:
  self-hosted runner" is replaced by the actual CI state — two `gpu-full`
  jobs exist but the only registered runner is labelled `sycl-arc` and
  `GPU_COVERAGE_ENABLED` is unset, so neither can be scheduled, and no job
  covers tiny-AI cross-device parity at all. Tracked as
  `T-GPU-RUNNER-LABEL-MISMATCH-2026-09-05` in
  [`docs/state.md`](../../docs/state.md).
