---
paths:
  - core/src/feature/sycl/integer_ssim_sycl.cpp
  - core/src/feature/sycl/sycl_integer_ssim_math.h
  - core/test/test_sycl_ssim_parity.c
invariant: integer_ssim_sycl = CPU ssim, bit for bit (ADR-1443); SSIM twins take CPU options.
---
<!-- markdownlint-disable MD013 MD060 -->
# Integer SSIM extractor and kernels

- **`integer_ssim_sycl.cpp` SSIM twins take CPU options; identical window
  = exactly 1** (ADR-1365). `integer_ssim_sycl`: `enable_db`, `clip_db`;
  `float_ssim_sycl`: `enable_lcs`, `enable_db`, `clip_db`, `scale`. dB
  conversion + ceiling on host (`vmaf_ssim_max_db`,
  `vmaf_ssim_emit_*_named` in `nonfinite_score.h`). Per-window formula:
  every product in named temporary, variances summed as pair,
  `numerator == denominator ? 1 : n / d`. Mirrored operation sequence ->
  identical window gives exactly 1 -> `enable_db` = CPU's `+inf` /
  `clip_db` ceiling (ADR-1221), not finite dB of fp32 residue.
  `enable_lcs` = separate kernel (`FloatSsimLcsKernel`; default path
  `FloatSsimTermKernel`), terms = `iqa/ssim_tools.c` L/C/S (clamped
  variances, flat-window covariance clamp, C3 = C2 / 2). Since ADR-1414
  per-pixel helpers (`ssim_float_parts`, `add_*_tap`, ...) live in
  `sycl_ssim_terms.h`, shared with MS-SSIM twin; sums: ADR-1463 bullet
  below.
  **On rebase**: do not fold products back into
  expressions (icpx contracts `a * b + c`, ADR-1358) or restore
  left-to-right four-term variance sum; identical-frame cases in
  `test_sycl_twin_option_parity` fail on either.
- **`integer_ssim_sycl` = CPU `ssim`, bit for bit (ADR-1443).** Supersedes
  fp32 formula of bullet above for fixed-point twin
  (`float_ssim_sycl` unchanged). CPU: `ssim_reduce_row_range()` forms each
  pixel's term in fp64 from int64 moments, `calc_ssim()` adds every term
  into ONE double in raster order. Twin: pass 1 = five int64 horizontal
  moment planes (no weight plane: weight = product of two tap sums,
  `tap_weight(tap_range())`; frame weight sum = `line_weight(w) *
  line_weight(h)` on host). Pass 2 = `IssimTermKernel`: vertical moments,
  then `vmaf_sycl_issim::term_bits()` (`sycl_integer_ssim_math.h`) =
  reference's fp64 operations one for one, in its order, on
  `SoftSigned` (sign, 53-bit significand, exponent in integers,
  `sycl_soft_signed.h`; RN ties-to-even per operation). Stores fp64
  BIT PATTERN per pixel (`uint64_t`), NO reduction on device. Host:
  `frame_sum_of_terms()` (shared with `float_ssim_sycl`) adds plane in
  index order. Three
  shortcuts, each exact: (a) window weight 2^16 (every window inside
  frame) -> product with it = exponent + 16 (`times_weight()`), edge
  windows take multiplication; (b) all six integer products below
  2^52 (always at 8 / 10 bit) -> four product sums are integers
  below 2^53, no rounding (`product_sums_exact()`), else
  `product_sums_rounded()` = each product rounded, sums left to right;
  (c) quotient = three radix-2^19 digits, digit estimated by fp32 division
  of top 24 bits, remainder exact in int64, two corrections each way
  (`soft_div_digits()`): device division accuracy does not matter.
  NEVER: fp32 term (3.1e-7 on 4K, overflow -> `invalid ratio` at 16
  bit), per-group reduction (1.1e-11 even in double), `k * (w * w)` for
  c1 / c2 (reference rounds twice), `w * (a * b)`. Zero has no sign in
  `SoftSigned` (terms go into sum). Shape: SIMD-16 + 256-entry register
  file (`ISSIM_TERM_SG`, `ISSIM_TERM_GRF`); SIMD-16 default file = 3072 B
  private + 1248 B spill, SIMD-32 spills at both -> wrong values on xe
  (ADR-1395). Term
  function `flatten, always_inline`, header functions
  `VMAF_SYCL_ALWAYS_INLINE`: call left in kernel = scratch frame.
  Arc A380: Netflix 48 / 48, checkerboards 3 / 3, BBB 4K 200 / 200, 10 /
  12 / 16 bit, 4:2:2, `enable_db` / `clip_db`: identical (vs GCC CPU:
  dB differs 3.6e-15 on 10 of 266 frames = host `log10`, libimf vs glibc).
  Cost 17.8 -> 31.9 ms / 4K frame (arithmetic 7.2, read-back 5.8, host
  adds 2.8): `T-SYCL-SSIM-EXACT-THROUGHPUT-2026-10-02`.
  `ssim_reduce_row_range()` lines change upstream -> change header
  same PR. Guards: `test_sycl_integer_ssim_math` (operations + term vs
  fp64, host + device), `test_sycl_ssim_parity` (+ `_large`, `==`, 15
  cases, 12 fail on old twin), `test_sycl_ssim_exact_contract.py`
  (13 planted regressions), `test_sycl_kernel_scratch`.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_ssim_sycl.cpp` | `integer_ssim.c` | `test_sycl_ssim_parity.c` (+ `_large`; bit-exact, 8 to 16 bit), `test_sycl_integer_ssim_math.c` | ADR-0884 (round 2), ADR-1443 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_ssim_sycl.cpp` (integer fex) | `test_sycl_ssim_parity.c` | ADR-0884 |
