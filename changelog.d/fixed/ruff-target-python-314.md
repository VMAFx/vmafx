- **Ruff now targets Python 3.14, the version the tooling requires.** The root `pyproject.toml`
  declared `requires-python = ">=3.14"` but told ruff to target Python 3.10, so ruff treated the
  standard-library `tomllib` as a third-party package and sorted its import into the wrong block in
  26 scripts and tests. The target is `py314` and the imports are re-sorted.
