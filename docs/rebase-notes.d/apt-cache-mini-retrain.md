## Apt archive cache for mini-retrain.yml (2026-10-23)

- The `Mini Retrain nightly` (`mini-retrain`) job in `mini-retrain.yml` caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier, and the hash of `.github/apt/mini-retrain.txt`.
- The install step carries a `timeout-minutes: 5` cap (sized at least 1.25x the slowest measured healthy run of 0.4m), with restore and save steps capped at 5 minutes. The job timeout limit remains at 40 minutes, covering the step caps sum (15) plus 1.25x the longest measured job (6.8m = 24 <= 40).
- `mini-retrain.yml` leaves the uncapped-apt exception list in `scripts/ci/check_job_timeout_budget.py`.
- **On sync**: edit package lists in `.github/apt/mini-retrain.txt` rather than inlining apt package names in `mini-retrain.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
