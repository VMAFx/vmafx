---
paths:
  - core/src/feature/sycl/float_psnr_sycl.cpp
  - core/test/test_sycl_float_psnr_parity.c
invariant: float_psnr_sycl = CPU float_psnr, bit for bit past 2^53 units too; 64-bit int row-segment sums.
---
<!-- markdownlint-disable MD013 MD060 -->
# Float PSNR extractor and kernels

- **`float_psnr_sycl` = CPU `float_psnr`, bit for bit
  ([ADR-1450](../../../../../docs/adr/1450-sycl-float-psnr-exact-block-sums.md)).**
  CPU: `diff * diff` in `float`, terms added in double per row (exact),
  rows in double (rounds past 2^53 units of 1 / scaler^2). Kernel:
  `fpsnr_pixel_noise()` = fp32 square of RAW integer difference, as
  `uint64` (same significand as CPU's term: power-of-two scaling);
  sub-group reduce, work-group total and read-back `uint64`; work-group =
  256 pixels of ONE row (`FPSNR_WG_X` 256, `FPSNR_WG_Y` 1). Host:
  `vmaf_float_psnr_row_noise()` (`feature/float_psnr_rows.h`: each row's
  groups in `uint64`, rows into double in order, ADR-1499), then
  `/ scaler^2 / n_pix`. NEVER
  fp32 or fp64 group sum (fp32 exact only to 24 bits: 2.4e-8 dB off on
  12-bit noise, 7.4e-8 on bright 16-bit), never integer square (CPU
  rounds square to 24 bits at 16 bit). Both helpers
  `VMAF_SYCL_ALWAYS_INLINE` (ADR-1395). 16 bit: CPU's own sum rounds past
  MSE x pixels = 2^37 (8-bit scale); twin rounds where it rounds. High-bit
  Netflix fixtures = 8-bit shifted left, show nothing: use full-range noise.
  Host `log10` = build's libm (`T-ICX-LIBIMF-HOST-MATH-2026-10-01`).
  Guards: `test_sycl_float_psnr_parity` (+ `_large`),
  `test_sycl_float_psnr_exact_contract.py`.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `float_psnr_sycl.cpp` | `float_psnr.c` | `test_sycl_float_psnr_parity.c` (+ `_large`; bit-exact, 8 to 16 bit, `uncapped`, past 2^53 too) | ADR-0946 (round 3), ADR-1450, ADR-1499 |
