- The four longest Linux jobs of master pushes (`Coverage Gate`, `Dev Container Build work`,
  `Docker Image Build work`, `FFmpeg SYCL work`) run on Depot runners when the repository
  variable `VMAFX_DEPOT_LINUX_RUNNER` holds a Depot label; unset, they run on GitHub-hosted
  runners as before, and pull requests always do. `scripts/ci/depot_minutes.py` reports the
  month's Depot base minutes (ADR-2168).
