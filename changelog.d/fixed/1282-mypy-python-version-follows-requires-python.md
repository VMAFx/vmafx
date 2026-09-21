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
