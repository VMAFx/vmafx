- CI: `Check Win64 stack alignment` in the `Windows UCRT64` job stops after 15 minutes
  instead of using up the job's 45. On master `190eea419` the step ran for 39 minutes
  (about 6 on the run before), and the job ended as cancelled. The check now prints its
  wall time and its three slowest files (`.github/workflows/libvmaf-build-matrix.yml`,
  `scripts/ci/check-win64-stack-alignment.py`).
