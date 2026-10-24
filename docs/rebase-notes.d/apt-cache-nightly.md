## Apt archive cache for nightly.yml (2026-10-23)

- The `Full clang-tidy scan` (`clang-tidy-full`) job in `nightly.yml` caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier, and the hash of `.github/apt/clang-tidy-full.txt`.
- The install step carries a `timeout-minutes: 5` cap (sized at least 1.25x the slowest measured healthy run of 2.3m), with restore and save steps capped at 5 minutes. The job timeout limit remains unchanged at 60 minutes, covering the step caps sum (15) plus 1.25x the longest measured job (21.1m).
- `nightly.yml` leaves the uncapped-apt exception list of `check_job_timeout_budget.py`.
- **On sync**: edit package lists in `.github/apt/clang-tidy-full.txt` rather than inlining apt package names in `nightly.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
