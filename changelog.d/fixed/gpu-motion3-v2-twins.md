- **GPU `motion_v2` twins emit `motion3_v2_score` (SYCL / HIP / Metal).** The
  SYCL, HIP, and Metal `motion_v2` twins now emit
  `VMAF_integer_feature_motion3_v2_score` and accept the
  `motion_blend_factor` / `motion_blend_offset` / `motion_max_val` /
  `motion_moving_average` options, mirroring the CUDA twin (ADR-1108) and the CPU
  reference byte-for-byte via the shared `motion_blend_tools.h` host helper. This
  closes the cross-backend follow-up ADR-1108 left open after the CUDA twin
  (#909); all four GPU backends now match the CPU `motion_v2` flush at default
  options. Host-side only — no GPU kernel change.
