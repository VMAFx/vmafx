---
paths:
  - scripts/ci/exact_twins.d/*
  - scripts/ci/cross_backend_calibration.py
  - core/test/test_sycl_exact_twins.c
invariant: Twins declared exact as group; bit-identical with CPU reference, measured at --precision max.
---
<!-- markdownlint-disable MD013 MD060 -->
# Exact GPU twins declaration

- **Twins declared exact as group
  ([ADR-1451](../../../../../docs/adr/1451-sycl-exact-twins-declared.md),
  `scripts/ci/exact_twins.d/`).** `adm`, `motion`, `motion_debug`,
  `motion_v2`, `psnr`, `float_ssim`, `float_ssim_lcs`, `cambi`: `sycl`
  listed -> gate tolerance 0. Basis per twin: integer sums on device + CPU's
  host arithmetic (`adm_sycl` ADR-1362, `motion_sycl` / `motion_v2_sycl`
  ADR-1371, `psnr_sycl` ADR-1365), CPU's window terms as CPU's doubles +
  host sums in CPU's raster order (`float_ssim_sycl`, ADR-1370 /
  ADR-1463; until ADR-1463 integer frame sums, exact only up to float
  rounding of mean), integer
  pipeline + exact top-K sum (`cambi_sycl`, ADR-1357). Rule: listed = by
  construction AND measured identical on full-range noise at 8 / 10 / 12 /
  16 bit, never on measurement alone. listed twin that drifts is FIXED,
  never given tolerance. `test_sycl_exact_twins` = `==` on every output,
  8 + 10 bit. NOT listed: `speed_chroma` (device `log2` correctly rounded,
  CPU = build's libm; identical on 333 frames, library-dependent), `ciede`
  (libm bound). With 11 twins exact by their own ADRs: 19 of 21 gate
  features. Default-model VMAF on A380 = CPU's on every frame measured.
