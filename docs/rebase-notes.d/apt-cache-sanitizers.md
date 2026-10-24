## Apt archive cache for sanitizers.yml (2026-10-23)

- The `asan-ubsan`, `tsan`, and `fuzz-nightly` jobs in `sanitizers.yml` cache `.deb` package archives in `${{ runner.temp }}/apt-archives`.
- `asan-ubsan` and `tsan` cache keys are keyed on runner OS, architecture, job keypart `sanitizers`, and the hash of both `.github/apt/sanitizers-base.txt` and `.github/apt/sanitizers-llvm.txt`.
- `fuzz-nightly` cache key is keyed on runner OS, architecture, job keypart `fuzz-nightly`, and the hash of `.github/apt/fuzz-nightly.txt`.
- Every `apt-get install` step carries a 5-minute timeout cap (measured max 0.7m for `asan-ubsan`, 3.6m for `tsan`, 0.8m for `fuzz-nightly`, floor 5m at 1.25x), with restore and save steps capped at 5 minutes each.
- Job timeout limits: `asan-ubsan` stays at 30 minutes (covering step caps sum 15 plus 1.25x 11.9m rest = 30), `tsan` stays at 35 minutes (covering step caps sum 15 plus 1.25x 13.4m rest = 32), and `fuzz-nightly` increases from 30 to 50 minutes (covering step caps sum 15 plus 1.25x 25.3m rest = 47).
- Restored archives are copied into `/var/cache/apt/archives` before `apt-get install`, and downloaded archives are copied back to `${{ runner.temp }}/apt-archives` after install.
- **On sync**: edit package lists under `.github/apt/sanitizers-base.txt`, `.github/apt/sanitizers-llvm.txt`, and `.github/apt/fuzz-nightly.txt` rather than inlining package names in `sanitizers.yml`; modifying the package list automatically computes a fresh hash and busts the cache.
