## Apt archive cache (2026-10-22)

- `MCP Smoke` and `Tiny AI` in `tests-and-quality-gates.yml` cache `.deb`
  package archives in `${{ runner.temp }}/apt-archives` keyed on OS,
  architecture, job identifier, and the hash of `.github/apt/<job>.txt`.
- Restored archives are copied into `/var/cache/apt/archives` before `apt-get install`,
  and downloaded archives are copied back to the cache directory after install.
  **On sync**: edit package lists in `.github/apt/mcp-smoke.txt` and
  `.github/apt/tiny-ai.txt` rather than inlining apt package names in the
  workflow file; modifying the package list automatically busts the cache key.
