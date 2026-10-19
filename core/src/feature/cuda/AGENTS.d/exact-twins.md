---
paths:
  - scripts/ci/exact_twins.d/adm.cuda
  - scripts/ci/twin-drift-check.sh
invariant: All declared exact twin extractors in CUDA maintain bit-identical output with CPU.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Twins declared exact as a group (ADR-1457)

- `scripts/ci/exact_twins.d/{motion,motion_debug,motion_v2,psnr,float_ssim,float_ssim_lcs,float_ms_ssim,float_ms_ssim_lcs,cambi}.cuda`
  -> gate tolerance 0.
- Basis per twin: integer sums on device + CPU's host arithmetic
  (`motion_cuda` / `motion_v2_cuda` ADR-1372 + ADR-1373, `psnr_cuda`
  ADR-1373); CPU's window arithmetic type for type + fp32 frame mean
  (`float_ssim_cuda` ADR-1399, `float_ms_ssim_cuda` ADR-1403); integer
  pipeline + exact top-K sum (`cambi_cuda` ADR-1379).
- Rule: listed = by construction AND measured identical on full-range noise
  at 8 / 10 / 12 / 16 bit + 40x40 to 64x64, never on measurement alone.
  Listed twin drifting -> FIX twin, never tolerance, never delist.
- `test_cuda_exact_twins` = `==` on every output, 640x480, 8 + 10 bit.
- `float_ssim_cuda`: frame sums in CPU raster order since ADR-1464 (mean
  differed: `T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02`).
  `float_ms_ssim_cuda`: per-block sums of l / c / s per scale differ from
  CPU same way, 4 of 8.32e6 noise frames at 176x176, one float step
  of `l` or `c` mean. State of its sums = its own paragraph above
  (ADR-1403 entry); fix = raster order as `float_ssim_cuda`, own PR
  (`fix/cuda-float-ms-ssim-raster-order-sum`).
- NOT listed: `ciede` (libm bound 1e-9), `speed_chroma` (libm bound 5e-6).
  `vif` listed by ADR-1462 (reads host log2 table, no device `log2f`).
