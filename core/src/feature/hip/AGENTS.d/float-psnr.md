---
paths:
  - core/src/feature/hip/float_psnr_hip.c
  - core/src/feature/hip/float_psnr_hip.h
  - core/src/feature/hip/float_psnr/float_psnr_score.hip
invariant: float_psnr_hip reproduces CPU reference bits identically, past 2^53 units too.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_psnr_hip = CPU bits (ADR-1440, ADR-1499, `EXACT_TWINS`)

Score = `float_psnr.c`'s bits at 8 / 10 / 12 / 16 bits (gfx1036: 178 of 178
frames, full-range noise included). Rebase-sensitive:

- CPU: float square of each difference, added in `double` = exact sum. Twin
  exact only if its sums are exact too.
- Kernel term: `fpsnr_square()` = `(uint32_t)((float)(ref - dis) *
  (float)(ref - dis))` = CPU term x scaler^2 (same mantissa; at 16 bits both
  round square to 24 bits). Not integer `d * d`: differs at 16 bits.
- Sums: `uint32` per wave and per block. <= 12 bits: one sum
  (256 * 4095^2 < 2^32). 16 bits (`bpc > 12u`): low 16 bits and rest
  added separately, each < 2^24; thread 0 puts them together into ONE
  `uint64` per block. Block = 256 pixels of ONE row (`FPSNR_BX` 256,
  `FPSNR_BY` 1, kernel and host); never blocks that span rows.
- Host: `vmaf_float_psnr_row_noise()` (`feature/float_psnr_rows.h`): each
  row's blocks in `uint64`, rows into double in CPU's order
  (ADR-1499), `/ (scaler * scaler)`, `/ n_pix`. CPU's bits past 2^53 units
  too (its adds of rows round there).
- Never float or double accumulator on device: fp32 rounds at 10+ bits
  once block's rms difference reaches 256 codes (was up to 7.6e-8 dB off);
  fp64 is exact but costs 2x frame time on gfx1036 (double shuffle = two).
- Guards: `test_hip_float_psnr_parity` + `_large` (device, `==`; cases of
  `core/test/float_psnr_twin_parity.h`, shared with CUDA / SYCL, past 2^53
  included), `test_hip_float_psnr_exact_contract.py` (planted regressions),
  `test_float_psnr_rows` (host).
