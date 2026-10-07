---
paths:
  - scripts/ci/exact_twins.d/adm.hip
  - scripts/ci/twin-drift-check.sh
invariant: All declared exact twin extractors in HIP must maintain bit-identical output with CPU.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Twins declared exact (ADR-1437, `scripts/ci/exact_twins.d/`)

`motion_hip` (+ `debug=true`), `motion_v2_hip`, `psnr_hip`,
`integer_ms_ssim_hip` (+ `enable_lcs`), `cambi_hip` = CPU bits, gate tolerance
0. Measured gfx1036: 178 frames, 480x270 to 3840x2160, 8 to 16 bits, 4:2:2,
full-range noise; options too. Already listed: `adm_hip`, `float_motion_hip`,
`psnr_hvs_hip`. Rebase-sensitive:
- Exact because integer on device + CPU helpers on host (motion ADR-1377,
  psnr ADR-1382, cambi ADR-1378) or CPU arithmetic type for type (ms_ssim
  ADR-1403). float reduction, host copy of CPU routine, or device
  libm call in any of them breaks listing: fix twin, never loosen.
- `integer_ms_ssim_hip`: per-scale fp64 sum in CPU's raster order on
  host since 2026-10-02 (mean differed on constructed frame; section
  above), mean rounded to fp32.
- NOT exact, do not list: `float_moment_hip` (exact integer squares; CPU rounds
  each square to float, differs at 16 bits; `float_psnr_hip` became exact
  under ADR-1440 below). Both identical on every real clip measured.
- Guard: `test_hip_exact_twins` (device, four frames, 8 and 10 bit, `==` on
  every output of all five). Gate unit tests:
  `scripts/ci/test_cross_backend_parity_gate.py`.
- Sweep table of every HIP twin:
  `docs/research/1437-hip-twin-exactness-sweep.md`.
