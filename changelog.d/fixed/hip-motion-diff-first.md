- **`motion_hip` now computes the CPU `motion` arithmetic (ADR-1377).** The
  HIP twin blurred each frame and differenced the blurred frames, while the
  CPU (since the upstream pipelined-motion port) blurs the frame difference
  and rounds after each filter pass; the two orders round differently, so
  `motion2` / `motion3` were up to 1.26e-5 off on the Netflix 576x324 pair on
  a gfx1036. `motion_hip` and `motion_v2_hip` now run one diff-first kernel,
  the one `motion_v2_hip` already used, and are expected to match
  `--backend cpu` bit for bit; the debug `motion` score now carries
  `motion_fps_weight` and `motion_max_val` like the CPU's, and a one-frame
  run reports `motion3 = 0`. Both motion twins copy the reference luma into
  pinned memory and upload it without a host wait in `submit()`. Measured on
  a gfx1036: `motion2` / `motion3` identical to `--backend cpu` on every
  frame (1.26e-5 apart before); at 4K `motion_hip` takes 12.95 ms per frame
  (14.25 before) and `motion_v2_hip` 13.24 (10.17 before), the staged upload
  costing more than the wait it removes on that iGPU; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).
