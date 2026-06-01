# Python type, dependency, and security audit bundle

This bundle consolidates four source PRs into a single coherent audit pass
across the fork-local Python trees (`ai/`, `mcp-server/vmaf-mcp/`,
`tools/vmaf-tune/`):

- **#326 — bandit MEDIUM sweep** (`fix(ai)`): Route `VMAF_TINY_AI_SCRATCH`
  through `tempfile.gettempdir()` instead of hardcoded `/tmp/` literals
  (bandit B108); annotate five `torch.load(weights_only=False)` call sites
  with `# nosec B614` plus trust-boundary citations; add `http(s)://`-scheme
  guards on `urllib.request.urlopen()` call sites (bandit B310); replace
  `/tmp/` literals in `ai/tests/` with `pytest` `tmp_path` fixture or neutral
  placeholders.

- **#366 — mypy --strict audit** (`chore(python)`): Tighten type annotations
  across `ai/src`, `mcp-server/vmaf-mcp/src`, and `tools/vmaf-tune/src`.
  Surfaces and fixes three real bugs: a duplicate `_run_benchmark()` definition
  (dead unreachable code), a `set[str]` holding tuples, and a loop-variable
  shadowing `CodecRow` / `LadderRung` access. Error counts: ai 7→0,
  mcp 16→0, tune 261→196 (residue deferred).

- **#369 — Python dependency floor refresh** (`chore(deps)`, ADR-0879): Bump
  nine stale floors: `optuna` (3.6→4.8.0, critical: aligns dev/runtime
  extras), `typer`, `anthropic`, `openai`, `pytest-asyncio`, `pytorch-lightning`,
  `mcp`, `ruff`, `pandas-stubs`. Add `pytest-cov>=7.1.0` floor in
  `python/test/requirements.txt`.

- **#377 — pyright --strict audit** (`chore(python)`, ADR-0888): Fix 12
  high-impact sites mypy missed — undefined `Tensor` forward-ref hidden by
  `# noqa: F821`, ORT-result union narrowing, `optuna` optional-import access,
  two always-true Optional comparisons, missing `CodecAdapter.presets` Protocol
  field, `TextIO` vs `object` for write stream, Optional-narrowing-through-raise
  gaps. Error counts: ai 370→306, mcp 61→61 (#366-owned), tune 1257→1236.
