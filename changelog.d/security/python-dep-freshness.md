- **Python dependency floors bumped to PyPI latest (ADR-0879)** — the
  ten most-stale `pyproject.toml` / `requirements.txt` floors across
  `ai/`, `mcp-server/vmaf-mcp/`, `dev-llm/`, `tools/vmaf-tune/`,
  `tools/vmaf-roi-score/`, and `python/test/` were lifted to match
  current PyPI releases (2026-05-30 snapshot). Highlights: `optuna`
  on `tools/vmaf-tune[dev]` moved from `>=3.6` to `>=4.8.0` (aligning
  with the existing `fast` extra), `anthropic` / `openai` /
  `typer` / `pytest-asyncio` each picked up one minor release of bug
  fixes, and the remaining bumps are patch / datestamp refreshes. No
  ceilings were touched; hash-pinning (`--hash=sha256:`) for the
  install-facing `requirements*.txt` files is deferred to a separate
  ADR cycle so the lockfile policy gets the deliberation it warrants.
