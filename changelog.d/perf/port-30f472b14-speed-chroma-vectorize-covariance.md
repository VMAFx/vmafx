### perf(simd): Speed_chroma vectorize compute_covariance (AVX2 + AVX-512, upstream 30f472b14)

- **UPSTREAM PORT** — `core/src/feature/x86/speed_avx2.c` + `speed_avx2.h`:
  AVX2 4-wide-double FMA accumulator with dual parallel chains for FMA latency
  hiding; processes 8 elements/iteration, then 4-lane tail, then scalar tail.
- `core/src/feature/x86/speed_avx512.c` + `speed_avx512.h`:
  AVX-512 8-wide-double FMA accumulator; processes 16 elements/iteration,
  then 8-lane tail, then scalar tail.
- `core/src/feature/speed.c`: added `compute_cov_kernel_fn` typedef,
  `compute_cov_kernel_scalar` reference kernel, kernel fn-pointer field on
  `SpeedState`, dispatch in `speed_init()` (AVX-512 > AVX2 > scalar).
- `core/src/meson.build`: `speed_avx2.c` wired into `x86_avx2_sources`
  (compiled with `-mavx2 -mfma`); `speed_avx512.c` wired into
  `x86_avx512_sources`.
- `core/test/test_speed_simd.c`: numerical-parity test (4 AVX2 + 4 AVX-512
  fixtures); tolerance 1e-9 relative (~500x tighter than snapshot gate).
  Wired into `test/meson.build` under suite `[fast, simd]`.
