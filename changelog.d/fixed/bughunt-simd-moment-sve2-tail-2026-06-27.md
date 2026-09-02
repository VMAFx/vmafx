Fix two SIMD `float_moment` correctness/bit-exactness bugs found by the
2026-06-27 bug-hunt sweep.

1. **SVE2 moment widening (wrong math).** `compute_1st_moment_sve2` /
   `compute_2nd_moment_sve2` (`core/src/feature/arm64/moment_sve2.c`) assumed
   the SVE `FCVT .s -> .d` (`svcvt_f64_f32`) widens the *lower contiguous*
   f32 lanes. Per the ARM A64 reference it actually reads the **even-indexed**
   f32 lanes (source element `2*i`). Stepping by `svcntd()` and using only
   `svcvt_f64_f32_x` therefore double-counted the even lanes and dropped every
   odd lane on any SVE register wider than 64 bits — emulated relative error up
   to ~45% at 128-bit VL. Fixed by stepping a full f32 register (`svcntw()`)
   and widening **both** halves: even lanes via `svcvt_f64_f32_x` and odd lanes
   via the SVE2 `svcvtlt_f64_f32_x` (FCVTLT, source element `2*i+1`), with
   merging adds (`svadd_f64_m`). Verified bit-correct vs the scalar reference
   under `qemu-aarch64 -cpu max` at SVE VL = 128/256/512 bits across 17 widths.

2. **2nd-moment SIMD tail squared in double (bit-exactness divergence).** The
   per-row scalar tail in `moment_avx2.c`, `moment_avx512.c`, and
   `moment_neon.c` squared in double (`(double)p * (double)p`) while the SIMD
   main loop and the scalar reference (`moment.c`: `pic_ * pic_`) square in
   float. Tail rows (width not a multiple of the SIMD lane count) thus diverged
   from scalar. Fixed to `(double)(p * p)`. Golden-safe: the Netflix
   `float_moment` golden tests use 576-wide content (a multiple of 8/16/4), so
   the tail never executes — the built binary reproduces the golden
   `float_moment_ref2nd` mean `4696.668388` exactly.

Regression coverage: new tail-only bit-exact tests in
`core/test/test_moment_simd.c` (`test_avx2_tail_bitexact`,
`test_avx512_tail_bitexact`, `test_neon_tail_bitexact`); the existing SVE2
relative-tolerance tests cover the FCVT fix on SVE2 hardware/emulation.
