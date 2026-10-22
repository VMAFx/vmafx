- **The `Dev Container Build` job no longer times out on a slow but healthy
  run.** Its limit rises from 75 to 90 minutes, at least 1.25 times the longest
  of 12 successful runs measured on 2026-10-22 (69.1 minutes). A run that was
  still compiling FFmpeg had been cancelled at 75 minutes
  (`.github/workflows/dev-container-build.yml`).
