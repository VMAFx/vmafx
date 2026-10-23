## Apt archive cache for fuzz.yml (2026-10-23)

- The `fuzz` job in `fuzz.yml` caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier, and the hash of `.github/apt/fuzz.txt`.
- The install step carries a `timeout-minutes: 5` cap (sized at least 1.25x the slowest measured healthy run of 1.5m), with restore and save steps capped at 5 minutes. The job timeout limit is raised from 30 to 40 minutes to cover the step caps sum (15) plus 1.25x the longest measured healthy job (19.2m = 39, rounded up).
- `fuzz.yml` leaves the uncapped-apt exception list of `scripts/ci/check_job_timeout_budget.py`.
- **On sync**: edit package lists in `.github/apt/fuzz.txt` rather than inlining apt package names in `fuzz.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
