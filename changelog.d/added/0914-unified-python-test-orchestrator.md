- **Unified Python test orchestrator via top-level `noxfile.py`
  ([ADR-0914](../docs/adr/0914-unified-python-test-orchestrator.md)).**
  Replaces the per-package venv-recipe-from-CI-YAML copy-paste loop with
  one command per suite: `nox -s ai`, `nox -s mcp`, `nox -s vmaf_tune`,
  `nox -s dev_llm`, `nox -s roi_score`, `nox -s ensemble_kit`,
  `nox -s python_harness`, plus the meta-sessions `nox -s all` and
  `nox -s lint`. Each per-package session creates a throw-away venv,
  installs the package with its `[dev]` extras, and runs `pytest <pkg>/tests/`;
  the `python_harness` session delegates to the existing `python/tox.ini`
  to preserve the Cython + Netflix golden-data setup. CI continues to
  drive each package's pytest through its own venv recipe in
  `tests-and-quality-gates.yml` — nox is a local-developer affordance,
  not a CI gate. Operator guide:
  [`docs/development/python-test-orchestrator.md`](../docs/development/python-test-orchestrator.md).
