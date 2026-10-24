- **The Python hash locks are regenerated with uv 0.13.5.** The tooling-test pin and `uv_version`
  in `requirements/locks/manifest.json` move together with the Renovate patch update (ruff 0.17.2,
  semgrep 1.182.1, uv 0.13.5); every lock is rewritten from its manifest entry
  (`make python-locks-write`), which also moves the transitive `starlette` pin to 1.10.0.
