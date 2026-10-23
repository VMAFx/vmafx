## Apt archive cache for build.yml (2026-10-23)

- The Linux base dependency step in `build.yml` (`build-work` job) caches `.deb` package archives in `${{ runner.temp }}/apt-archives` keyed on OS, runner architecture, job name, matrix OS, and the hash of `.github/apt/build-linux.txt`.
- Every `apt-get install` step in `build.yml` carries a `timeout-minutes` cap sized at or above 1.25 times its slowest measured healthy run ("Install base deps (Linux)" at 26m, "Install Intel oneAPI (Linux)" at 10m, "Install ROCm / HIP toolchain (Linux)" at 5m, "Install base deps for CUDA leg (Linux)" at 10m).
- The `build-work` job limit is raised from 75 to 80 minutes to cover the sum of its step caps plus 1.25 times the measured duration of uncapped steps (`scripts/ci/check_job_timeout_budget.py`).
- Restored archives are copied into `/var/cache/apt/archives` before `apt-get install`, and newly downloaded archives are copied back to `${{ runner.temp }}/apt-archives` after install.
- **On sync**: edit package lists under `.github/apt/build-linux.txt` rather than inlining package names in workflow `apt-get install` commands; modifying the package list automatically computes a fresh hash and busts the cache.
