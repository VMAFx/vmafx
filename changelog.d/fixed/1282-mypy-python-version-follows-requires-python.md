- `mypy` now models the Python version the project actually requires
  (ADR-1282). `[tool.mypy] python_version` in `pyproject.toml` was
  pinned to `3.10` while `requires-python` declared `>=3.14` and every
  CI leg installs 3.14.7. That was not only inaccurate — it stopped the
  check: mypy refuses to parse a PEP 695 `type` statement when told to
  target below 3.12, numpy's bundled `__init__.pyi` contains one, and
  the resulting blocking `[syntax]` error aborted the `ai/src/` pass
  with *"errors prevented further checking"* after checking zero source
  files in any checkout with numpy installed. At `3.14` that pass runs
  to completion (`checked 40 source files`) and 60 pre-existing findings
  across `ai/` and `scripts/` become visible for the first time.
  Measured against a merge base carrying the same value, the raise
  introduces no findings of its own — the debt was hidden, not created —
  and none of it is suppressed here. `[tool.black]` and `[tool.ruff]`
  `target-version` carry the same staleness and are left to their own
  change because moving them rewrites code.
  The pairing is now enforced instead of asserted in a comment:
  `scripts/ci/check-workflow-versions.py` fails the always-run
  pre-commit gate when `[tool.mypy] python_version`, the
  `requires-python` floor and `PYTHON_CI_VERSION` stop agreeing, or when
  the pin is deleted rather than reverted
  (`scripts/ci/tests/test_mypy_python_version_single_source.py`).
  Scope correction: CI's advisory `Python Lint` job installs only
  `mypy`, so numpy's stubs are never read there. That job's output is
  byte-identical at `3.10` and `3.14` and it checked — and still
  checks — zero source files, because with no dependencies installed
  every import is unresolvable and the run aborts before semantic
  analysis. The behaviour change here is confined to checkouts that
  have the AI stack installed, which includes the local pre-push hook's
  environment.
