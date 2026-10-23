## Apt archive cache for go-ci.yml (2026-10-23)

- The `go-ci` job in `go-ci.yml` caches `.deb` package archives in
  `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier,
  and the hash of `.github/apt/go-ci.txt`.
- The apt install step has a `timeout-minutes` cap of 9 (at least 1.25x the
  slowest measured healthy run of 6.6m), and the job limit is set to 90
  minutes to cover the step caps sum (5+9+5 = 19) plus 1.25x the 47m rest
  (`scripts/ci/check_job_timeout_budget.py`).
- Restored archives are copied into `/var/cache/apt/archives` before
  `apt-get install`, and downloaded archives are copied back to the cache
  directory after install.
- **On sync**: edit package lists under `.github/apt/go-ci.txt` rather than
  inlining package names in workflow `apt-get install` commands; modifying the
  package list automatically computes a fresh hash and busts the cache.
