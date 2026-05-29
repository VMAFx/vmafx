# Research digest: AVX-512 motion parity test coverage gap (2026-05-29)

## Summary

Prior to this PR, the six AVX-512 motion kernels had no direct unit tests.
The gap was identified during the SIMD intrinsics code review initiated in
the 2026-05-29 session.

## Findings

### Coverage gap

`test_motion_v2_simd.c` exercised `motion_score_pipeline_16_avx2` via the
AVX2 dispatch gate.  The corresponding AVX-512 functions in
`motion_v2_avx512.c` and `motion_avx512.c` were not referenced by any test
file.  The Netflix golden-data gate (`python/test/`) operates at full-frame
granularity and has no ability to isolate a per-kernel regression.

### Shift correctness audit

The AVX2 path used `_mm256_srlv_epi64` (logical right shift) for the
`>> bpc` step in `motion_score_pipeline_16_avx2`.  The AVX-512 twin uses
`_mm512_srav_epi64` (arithmetic right shift), which matches the scalar C
`>>` on `int64_t` exactly.  The negative-diff adversarial fixture in the
new test confirms this is correct on the host.

### Scalar reference strategy

`y_convolution_8`, `y_convolution_16`, `x_convolution_16`, and `sad_c`
have `static` linkage in `integer_motion.c`.  Local scalar reimplementations
are provided in the test file, mirroring the existing precedent in
`test_motion_v2_simd.c` for the pipeline functions.

## Conclusion

The ten test cases added by this PR provide direct bit-exactness coverage
for all six AVX-512 motion kernels and serve as a permanent regression
guard.  No kernel-level divergences were found during the audit.
