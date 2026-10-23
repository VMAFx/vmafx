## Apt archive cache for lint-and-format.yml (2026-10-23)

- The `clang-tidy`, `clang-tidy-ratchet`, `clang-tidy-sycl`, and `cppcheck` jobs in `lint-and-format.yml` cache `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on runner OS, runner architecture, job key, and package list hash.
- Package requirements are declared in `.github/apt/tidy-changed.txt`, `.github/apt/tidy-ratchet.txt`, `.github/apt/tidy-sycl.txt`, and `.github/apt/cppcheck.txt`.
- Each apt install step is capped with `timeout-minutes: 5` (at least 1.25x slowest measured run), and restore and save cache steps are capped at 5 minutes each.
- Job timeout limit for `cppcheck` is increased from 30 to 45 minutes to cover the 15-minute step cap sum plus 1.25x the measured uncapped steps (24m). Limits for `clang-tidy` (25m), `clang-tidy-ratchet` (45m), and `clang-tidy-sycl` (40m) stay unchanged.
- Restored archives are copied into `/var/cache/apt/archives` before `apt-get install`, and downloaded archives are copied back to the cache directory after install.
- **On sync**: edit package lists under `.github/apt/` rather than inlining apt package names in `lint-and-format.yml`; modifying a list file automatically computes a fresh hash and busts the cache.
