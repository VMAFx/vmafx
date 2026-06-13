- **Stale-marker sweep 2026-05-08 — full-tree audit
  ([Research-0086](docs/research/0086-stale-marker-sweep-2026-05-08.md))** —
  classified every `@pytest.mark.skip` / `@unittest.skip` / `pytest.skip` /
  `_*_DEFERRED` / `raise NotImplementedError` / `// TODO|FIXME|XXX` /
  `return -ENOSYS` / `#error "unimplemented"` marker in fork-touched paths
  (`tools/`, `python/vmaf/`, `ai/`, `mcp-server/`, `core/src/`,
  `core/test/`). Outcome: ~95 markers DEFERRED-VALID with documented
  reopen triggers (HIP T7-10b scaffolds per ADR-0212/ADR-0274, MCP T5-2b
  scaffold per ADR-0209, `vmaf-tune fast` production wiring per
  ADR-0276 / PR #467, `vmaf-roi-score` T6-2c per ADR-0288, environment-
  gate test skips, abstract-base-method `NotImplementedError` patterns).
  One marker — `_HDR_ITER_ROWS_DEFERRED` in
  `tools/vmaf-tune/tests/test_hdr.py` — was missing a cross-link to its
  follow-up PR (#466 HP-2) and to `docs/state.md`; this PR adds both and
  opens a `T-HDR-ITER-ROWS` row under "Deferred" so the un-skip rides
  with the wiring PR. Zero TODO/FIXME/XXX comments remain in fork-added
  Python (`tools/`, `ai/`, `mcp-server/`); the seven survivors are all in
  upstream-mirrored files, untouched by policy.
