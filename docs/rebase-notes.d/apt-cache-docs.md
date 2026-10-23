## Apt archive cache for docs.yml (2026-10-23)

- The `Doxygen Public API work` (`doxygen-work`) job in `docs.yml` caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier, and the hash of `.github/apt/doxygen-work.txt`.
- The install step carries a `timeout-minutes: 5` cap (sized at least 1.25x the slowest measured healthy run of 3.4m), with restore and save steps capped at 5 minutes. The job timeout limit is raised from 10 to 20 minutes to cover the step caps sum (15) plus 1.25x the longest measured job (3.7m).
- The stale `doxygen-public-api.yml` entry leaves the uncapped-apt exception list: that workflow was folded into `docs.yml`.
- **On sync**: edit package lists in `.github/apt/doxygen-work.txt` rather than inlining apt package names in `docs.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
