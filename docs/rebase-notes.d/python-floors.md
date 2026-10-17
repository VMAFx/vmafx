## Python floors follow numpy (2026-10-16)

- `python/pyproject.toml`, `ai/pyproject.toml`, `dev-llm/pyproject.toml`,
  `mcp-server/vmaf-mcp/pyproject.toml`, `tools/vmaf-roi-score/pyproject.toml`:
  floors raised to `>=3.12` to match `numpy>=2.5.3`. `tools/vmaf-tune/pyproject.toml`
  raised to `>=3.11` to match `matplotlib>=3.11.2`. `tools/rc1-tester` stays
  at `>=3.10`.
- Tool targets (`[tool.ruff] target-version`) in `ai`, `dev-llm`, and
  `mcp-server/vmaf-mcp` updated to `py312`.
- `scripts/ci/check_python_floors.py`: new automated check verifying that all
  tracked packages resolve at their declared floors with `--all-extras` via
  `uv pip compile` (`make python-floors-check`; [ADR-3335](../adr/3335-python-floors-match-dependencies.md)).
- Documentation updated in `docs/usage/python.md`, `docs/mcp/http-transport.md`,
  and `docs/development/python-dependency-locks.md`.
