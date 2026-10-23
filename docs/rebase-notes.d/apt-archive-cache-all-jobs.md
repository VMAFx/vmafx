## Apt archive cache for every apt job (2026-10-23)

- All jobs in `tests-and-quality-gates.yml` that run `apt-get install`
  (`netflix-golden`, `compat-abi-parity`, `sanitizers`, `dnn`, `tooling-tests`,
  `vmaf-tune-tests`, `mcp-smoke`, `coverage`) cache `.deb` package archives in
  `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier,
  and the hash of `.github/apt/<job>.txt`.
- Every apt install step has a `timeout-minutes` cap (sized at least 1.25x the
  slowest measured healthy run), and each job limit is sized to cover the sum
  of its step caps plus 1.25x the measured uncapped steps
  (`scripts/ci/check_job_timeout_budget.py`).
- Restored archives are copied into `/var/cache/apt/archives` before
  `apt-get install`, and downloaded archives are copied back to the cache
  directory after install.
  **On sync**: edit package lists under `.github/apt/<job>.txt` rather than
  inlining package names in workflow `apt-get install` commands; modifying the
  package list automatically computes a fresh hash and busts the cache.
