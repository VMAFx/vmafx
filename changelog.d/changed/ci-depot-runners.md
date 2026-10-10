- The four longest Linux jobs of master pushes (`Coverage Gate`, `Dev Container Build work`,
  `Docker Image Build work`, `FFmpeg SYCL work`) run on a paid fast-runner provider (Depot today) when the repository
  variable `VMAFX_FAST_LINUX_RUNNER` holds a runner label; unset, they run on GitHub-hosted
  runners as before, and pull requests always do. `scripts/ci/fast_runner_minutes.py` reports the
  month's Depot base minutes (ADR-2168).
