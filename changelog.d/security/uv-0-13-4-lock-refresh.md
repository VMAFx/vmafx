- **The Python hash locks are regenerated with uv 0.13.4.** uv 0.13.1 is affected by the critical
  advisory GHSA-hq85-5cpj-mr62 (Dependabot alerts 19 and 20, fixed in 0.13.4). The tooling-test pin
  and `uv_version` in `requirements/locks/manifest.json` move together, every lock is rewritten from
  its manifest entry (`make python-locks-write`), which moves six transitive pins forward
  (`setuptools`, `virtualenv`, `onnx-ir`, `httpx2`, `httpcore2`, `pydantic-settings`), and the
  `ai` package floor for Pillow rises to 13.0.0. `mcp` stays at 1.29.0 under its declared
  exception: `semgrep` 1.182.0, the newest release, still pins it exactly.
