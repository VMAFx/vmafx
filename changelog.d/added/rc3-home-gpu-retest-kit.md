- **RC3 home GPU retest kit** (`scripts/dev/rc3-home-gpu-retest.sh`): runs the
  verify-and-time commands that the `docs/state.md` rows carry for the RTX 4090
  (CUDA), the Arc A380 (SYCL) and the gfx1036 iGPU (HIP), one entry per row,
  with every device run under that device's lock. It writes a log and the JSON
  of every run per row plus a summary table, and `--baseline DIR` compares a
  pull request's run with an earlier run on `master`. See
  [the retest guide](docs/development/rc3-home-gpu-retest.md) (ADR-1386).
