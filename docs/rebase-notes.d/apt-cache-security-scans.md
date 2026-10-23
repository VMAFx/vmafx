## Apt archive cache for security-scans.yml (2026-10-23)

- The `CodeQL (C/C++)` (`codeql-cpp`) job in `security-scans.yml` caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, architecture, job identifier, and the hash of `.github/apt/codeql-cpp.txt`.
- The configure step carries a `timeout-minutes: 5` cap (sized at least 1.25x the slowest measured healthy run of 0.7m), with restore and save steps capped at 5 minutes. The job timeout limit is raised from 35 to 55 minutes to cover the step caps sum (15) plus 1.25x the measured uncapped steps (31.2m).
- Restored archives are copied into `/var/cache/apt/archives` before `apt-get install`, and newly downloaded archives are copied back to the cache directory after install.
- **On sync**: edit package lists in `.github/apt/codeql-cpp.txt` rather than inlining apt package names in `security-scans.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
