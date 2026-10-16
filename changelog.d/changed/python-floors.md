- **Python package floors match dependency resolutions ([ADR-3335](docs/adr/3335-python-floors-match-dependencies.md)).**
  Declared `requires-python` floors are raised to the lowest version where core
  and all optional extras successfully resolve (`python/`, `ai/`, `dev-llm/`,
  `mcp-server/vmaf-mcp/`, and `tools/vmaf-roi-score/` to `>=3.12`; `tools/vmaf-tune/` to
  `>=3.11`), matching requirements such as `numpy>=2.5.3` and `matplotlib>=3.11.2`.
  Tool targets (`[tool.ruff] target-version`) follow the package floors. The
  automated check `scripts/ci/check_python_floors.py` (`make python-floors-check`)
  verifies each package at its floor.
