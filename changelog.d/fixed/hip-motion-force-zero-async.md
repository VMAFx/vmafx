- **`motion_hip` and `float_motion_hip` no longer crash with
  `motion_force_zero=true`.** Both HIP twins switched to their synchronous
  zero path inside `init()` and cleared `submit()` / `collect()`, but libvmaf
  had already chosen the asynchronous path for them, so the first frame
  called a NULL `submit()` and the process died with SIGSEGV. The twins now
  keep the asynchronous interface and write the CPU's zeros from
  `collect()`. Measured on a gfx1036: `--feature motion_hip=motion_force_zero=true`
  exits 0 with every `integer_motion*_force_0` score 0, as on the CPU
  (`T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30` in `docs/state.md`).
