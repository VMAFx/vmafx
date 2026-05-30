- Pyright strict audit of the three fork-local Python trees
  (`ai/src`, `mcp-server/vmaf-mcp/src`, `tools/vmaf-tune/src`),
  companion pass to PR #366 (`mypy --strict`). Catches a different
  class of bug than mypy: cross-procedural `Optional` narrowing
  through `raise`, dead `is None` / `is not None` branches, missing
  Protocol fields, undefined forward-refs masked by `# noqa: F821`,
  ORT-result union narrowing. Fixes 12 high-impact sites — most
  notably an undefined-`Tensor` forward-ref in
  `ai/src/vmaf_train/confidence.py` (4 occurrences hidden behind
  `# noqa: F821`), the missing `presets` field on the
  `CodecAdapter` Protocol (declared on every concrete adapter and
  consumed by `ladder._default_sampler_preset` but absent from the
  contract), and a cross-procedural `optuna` Optional access in
  `tools/vmaf-tune/src/vmaftune/fast.py::_run_tpe`. Per-package
  strict-error count drops: `ai/src` 370 → 306 (-64), `mcp/src`
  61 → 61 (residue owned by PR #366), `tune/src` 1,257 → 1,236
  (-21). Most remaining errors are `reportUnknown*Type` cascades
  from third-party packages without stubs (torch, scipy, optuna,
  onnxruntime, pyarrow) — noise, not fork-code defects. See
  [ADR-0888](docs/adr/0888-pyright-strict-audit.md) and the
  research digest
  [`docs/research/pyright-strict-audit-2026-05-30.md`](docs/research/pyright-strict-audit-2026-05-30.md).
