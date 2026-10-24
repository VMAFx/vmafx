## Apt archive cache for rust-ci.yml (2026-10-23)

- The `vmafx-sys CI work` (`rust-vmafx-sys-work`) job in `rust-ci.yml` caches
  `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS,
  architecture, job identifier, and the hash of `.github/apt/rust-vmafx-sys.txt`.
- The apt install step has a `timeout-minutes: 16` cap (sized at least 1.25x
  the slowest measured healthy run of 12.7m), with restore and save steps capped
  at 5 minutes. The job timeout limit stays at 30 minutes, covering the step cap
  sum (26) plus 1.25x the measured uncapped steps.
- Restored archives are copied into `/var/cache/apt/archives` before
  `apt-get install`, and downloaded archives are copied back to the cache
  directory after install.
- **On sync**: edit package lists in `.github/apt/rust-vmafx-sys.txt` rather than
  inlining apt package names in `rust-ci.yml`; modifying the package list
  automatically computes a fresh hash and busts the cache.
