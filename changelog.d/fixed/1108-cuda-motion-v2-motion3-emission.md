### Fixed

- **CUDA `motion_v2_cuda` now emits `motion3_v2_score`**: the CUDA twin of the
  `motion_v2` extractor previously emitted only `motion_v2_sad_score` and
  `motion2_v2_score`, while the CPU reference (`integer_motion_v2.c`) also emits
  `motion3_v2_score` (the perceptually blended + clipped score, with an optional
  two-frame moving average). The CUDA twin's option table also lacked the four
  `motion3`-driving options. A `motion_v2_cuda` run that requested
  `motion3_v2_score` — a CHUG re-extract, a model file carrying
  `motion_v2=motion_blend_factor=…`, or a co-scheduled CPU+CUDA parity run —
  silently received no feature on the CUDA path. The CUDA `flush()` now computes
  `motion3_v2_score` host-side over the kernel's SAD scores using the identical
  per-frame `motion_blend` + `motion_max_val` clip + `stamp_value` seeding +
  optional moving-average formula as the CPU reference (via the shared
  `motion_blend_tools.h` helper), and the twin gains the `motion_blend_factor`,
  `motion_blend_offset`, `motion_max_val`, and `motion_moving_average` options
  mirroring the CPU `VmafOption` table byte-for-byte. Measured CPU-vs-CUDA
  parity on the Netflix `src01_hrc00`↔`src01_hrc01` 576×324 pair (48 frames,
  RTX 4090) is `max_abs` per-frame diff = `0.000e+00` at default options and at
  `motion_blend_factor=0.5 + motion_moving_average=1` (places=4, ADR-0214). The
  SYCL, HIP, and Metal twins carry the same gap and are tracked as follow-ups.
  (ADR-1108, supersedes the ADR-0337 GPU-twin deferral for CUDA)
