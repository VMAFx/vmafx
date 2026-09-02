### fix(simd): 16-bit PSNR scalar-tail signed-integer overflow in AVX2, AVX-512, and NEON

The scalar-tail loops in `psnr_sse_line_16_avx2`, `psnr_sse_line_16_avx512`, and
`psnr_sse_line_16_neon` computed the squared error as `(int32_t)e * (int32_t)e` where
`e` can reach ±65535. The product 65535² = 4 294 836 225 exceeds INT32_MAX, invoking
signed-integer overflow — undefined behaviour under C99/C11 that UBSan flags and that
can produce wrong SSE values on optimising compilers.

Fix: replace the signed multiply with `(uint64_t)e * e` using unsigned `abs`-diff,
mirroring the `sse_line_16_c` reference in `core/src/feature/integer_psnr.c`.
