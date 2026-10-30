- **The Python hash locks are regenerated with uv 0.13.6.** The tooling-test pin and `uv_version`
  in `requirements/locks/manifest.json` move together with the Renovate patch update (ruff 0.17.3,
  tox 4.67.2, uv 0.13.6); every lock is rewritten from its manifest entry
  (`make python-locks-write`), which also moves the transitive pins released since the last
  refresh, among them click 8.5.0, mypy 2.4.1 and the CUDA 13.2 wheels.
