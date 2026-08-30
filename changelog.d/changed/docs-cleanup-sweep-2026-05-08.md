- **Docs cleanup sweep (2026-05-08)** — repaired ~57 broken internal
  links from ADR slug-drift, normalised `../docs/...` paths in
  `CHANGELOG.md` (the file lives at the repo root), copied the missing
  upstream `resource/images/` assets (`CI.png`, `scatter_training.png`,
  `scatter_testing.png`) into the tree so the embeds in
  `docs/usage/python.md` and `docs/metrics/confidence-interval.md`
  resolve, refreshed the stale "Phase A scaffold" framing in
  `tools/vmaf-tune/README.md` and the top of `docs/usage/vmaf-tune.md`
  to reflect the current 17 codec adapters + 8 subcommands, replaced
  references to removed `core/src/cuda/ring_buffer.c` /
  `core/test/test_ring_buffer.c` with their current equivalents,
  added a new [`docs/api/mcp.md`](../docs/api/mcp.md) page covering
  `libvmaf_mcp.h`, regenerated `docs/adr/README.md` from
  `_index_fragments/` (70 missing fragments authored, 2 misnamed
  orphans renamed to match their on-disk ADR), and removed two
  dead external links in `docs/usage/external-resources.md`. Source:
  [`docs/research/0088-docs-cleanup-punch-list.md`](../docs/research/0088-docs-cleanup-punch-list.md).
