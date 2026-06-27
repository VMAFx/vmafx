- `python/tox.ini` test env bumped `py311` → `py314` to match the CI Python
  (`actions/setup-python` installs 3.14.5). The fork's Python deps already
  require ≥3.12 (`numpy>=2.4.6`, `scipy>=1.18.0`, `pandas>=3.0.3`), so the stale
  `py311` env could not resolve them — `pip install scipy>=1.18.0` failed with
  "No matching distribution" on 3.11 (`scipy` 1.18 is Python-≥3.12-only),
  reddening the (non-required) macOS Build job's tox step on every PR. The
  required Netflix golden gate is unaffected — it runs `pytest` directly on the
  CI Python, not through tox.
