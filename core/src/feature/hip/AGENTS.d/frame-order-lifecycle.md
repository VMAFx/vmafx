---
paths:
  - core/src/feature/hip/integer_adm_hip.c
  - core/src/feature/hip/integer_cambi_hip.c
invariant: Maintain strict frame lifecycle order of upload, clear, and kernel execution.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Frame order: upload, clear, kernels (ADR-1427)

Every HIP twin, every frame: `vmaf_hip_plane_source_acquire*()` /
`vmaf_hip_picture_upload*()` FIRST, then `hipMemsetAsync` of accumulators,
then kernels. Rebase-sensitive:

- Clear queued ahead of upload = lost on gfx1036 in first context of
  process needing larger planes than earlier contexts. Kernels add onto
  earlier context's sums in recycled device memory. Was: `adm_hip` (run
  fails), `float_moment_hip` (moments +3 %), `vif_hip` (scores -0.02).
  `vmaf` CLI = one context per process, fresh memory zero, never shows.
- Clear at allocation = no fix. `hipMemset` on device memory is async on
  null stream (hipamd `ihipMemset()`, ROCm 7.2.4); `float_moment_hip` stays
  wrong with it.
- Rule holds through helpers: helper that clears counts as clear at its
  call site, helper that uploads as upload.
- New twin: add row to `cases[]` in `core/test/test_hip_first_frame_clear.c`
  AND its name to `hip_first_frame_twins` in `core/test/meson.build` (one
  binary per twin; only first larger context of process is exposed).
- Guards: `test_hip_first_frame_clear_<twin>` (device),
  `test_hip_clear_after_upload_contract.py` (device-free, every `*.c` under
  `core/src/feature/hip/` and `core/src/hip/`, six planted regressions).
