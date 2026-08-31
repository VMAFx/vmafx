- Renovate: group `numpy` bumps across all managers (pep621 + pip_requirements
  + setuptools/`setup.py`) into a single PR. numpy is pinned in `ai/`,
  `mcp-server/`, `python/`, `ensemble-kit/` and `roi-score/` via a mix of
  `pyproject.toml`, `setup.py` and `requirements*.txt`, so a single bump was
  previously split into two PRs; the new `numpy (all managers)` package rule
  keeps them together.
