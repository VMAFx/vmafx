---
paths:
  - core/src/feature/hip/integer_ms_ssim_hip.c
  - core/src/feature/hip/integer_ms_ssim/ms_ssim_arith.h
invariant: float_ms_ssim_hip matches CPU arithmetic bit for bit with raster-order per-scale sums.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_ms_ssim_hip = CPU arithmetic, bit for bit (ADR-1403)

Scores = `float_ms_ssim.c`'s bits (16 outputs with `enable_lcs`; measured on
gfx1036: Netflix 8/10-bit, 1080p checkerboards, BBB 4K, 1712 of 1712 values).
Rebase-sensitive:

- All sample arithmetic lives in `integer_ms_ssim/ms_ssim_arith.h` (plain C +
  HIP C++). `ms_ssim_score.hip` and `integer_ms_ssim_hip.c` call it and own no
  `fmaf` / `sqrtf` / `pow` / multiply-accumulate. Change header, never
  copy.
- Decimate: one `fmaf()` per tap, row sums then column
  (`ms_ssim_decimate.c::vmaf_fmaf_exact()`). Not `acc += a * b`: contraction is
  off (ADR-1407), so that rounds twice.
- Window sums (`iqa_convolve()`): fp32 product per tap, summed as exact
  fp32 pair (`VmafHipMsPair`, two-sum, six fp32 ops), rounded once per pass.
  Stands for CPU's fp64 sum. fp64 accumulator = same scores, 299 vs 173 ms
  per 4K frame on gfx1036; plain fp32 sum = wrong on about every second
  window.
- l / c / s (`ssim_accumulate_default_scalar()`): fp32 variances with
  `MAX(0.0, x)`, fp64 numerator over fp32 denominator for l and c, s = fp32
  quotient. Constants fp32 (`vmaf_hip_ms_ssim_constants()`), passed to
  kernel as doubles and narrowed back (item 5 above still holds).
- Host: `vmaf_hip_ms_ssim_scale_mean()` rounds each mean to fp32,
  `vmaf_hip_ms_ssim_combine()` = `ms_ssim.c` product with `fabs()` on l, c, s.
- CPU change to `ms_ssim_decimate.c`, `iqa/convolve.c`, `iqa/ssim_tools.c`
  (default accumulate) or `ms_ssim.c` (combine) -> same change in header,
  same PR.
- Guards: `test_hip_ms_ssim_arith` (device-free replay vs CPU extractor, 8 and
  10 bit, odd size; two-sum exactness), `test_hip_ms_ssim_parity` +
  `_large` (device, `==`, 48 outputs), `test_hip_kernel_source_contract.py`
  (10 planted regressions).

## float_ms_ssim_hip per-scale sums = CPU raster order (2026-10-02)

`T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02`, HIP part; ADR-1438
construction. Rebase-sensitive:

- CPU `iqa/ssim_tools.c` `ssim_accumulate_default_scalar()`: `l`, `c`, `s` of
  every window into one double per sum, raster order, mean rounded to fp32.
  Another order = another double. fp32 rounding of mean does NOT absorb
  it: pair in `core/test/float_ms_ssim_order_frame.h` (176x176, found in 5.3e6
  noise frames) gave `float_ms_ssim_c_scale1` `0x3f7c499f` with old
  per-wave / per-block sum, CPU `0x3f7c49a0`; score moved 1.3e-9.
- Kernel: `terms[window]`, `terms[windows + window]`,
  `terms[2 * windows + window]`, `window = y * w_final + x`. No `__shared__`,
  no `__shfl_down`, no block sum.
- Host: `ms_ssim_hip_scale_sums()` adds `j = 0 .. windows - 1`, three chains
  in one pass. Do not reorder, vectorise across `j`, or split per row.
- Pinned host planes = `hipHostMallocDefault`. Write-combined = uncached
  reads, host loop reads every double.
- Fixture header: bytes shared with other backends' twin tests, never edit;
  luma only (chroma any value).
- Guards: `test_hip_ms_ssim_parity` (`test_ms_ssim_frame_sum_order`, device,
  16 outputs by bits + CPU premise without device),
  `test_hip_kernel_source_contract.py` (6 planted regressions).
- Cost: readback 24 bytes per window per scale (262 MB per 4K frame, on
  device and pinned). Tuning row
  `T-HIP-FLOAT-MS-SSIM-EXACT-THROUGHPUT-2026-10-02`.
