- Dispatch the SpEED dense matrix product through new bit-exact AVX2 and
  AVX-512 kernels (`speed_matmul_avx2` / `speed_matmul_avx512`), selected at
  runtime from the CPU-flag mask exactly like the existing
  `compute_cov_kernel_*` family and honouring `--cpumask`. `matrix_mul` in
  `core/src/feature/speed.c` was 20.78 % of a `perf` profile of the default
  `vmaf_v1.0.16_3d0h` model on the Netflix src01 pair, running on 128-bit
  vectors because the generic C library compiles at the x86-64 baseline; it is
  now 5.67 %, and the run is 1.20x faster (median of 9–11 interleaved
  repetitions on a near-idle host, 1-minute load average 2.8–4.1). Scores are
  unchanged: the widened axis is an output index rather than a reduction axis, both
  translation units compile `-ffp-contract=off` so no FMA fusion collapses the
  separate multiply and add, and `--precision=max` output is byte-identical
  before and after on the scalar, AVX2 and AVX-512 dispatch paths.
  `core/test/test_speed_simd.c` gates the twins with `memcmp` equality against
  the scalar reference. Models without a SpEED feature (`vmaf_v0.6.1.json`)
  are unaffected. See ADR-1196 and research digest 2030.
