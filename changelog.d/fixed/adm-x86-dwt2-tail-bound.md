### Fixed

- `core/src/feature/x86/adm_avx2.c`, `core/src/feature/x86/adm_avx512.c`: fix horizontal
  DWT2 tail loop boundary condition for AVX2 and AVX-512 kernels. Previously,
  the vector loop bound `((w + 1) / 2) - ((((w + 1) / 2) - 1) % N)` allowed the SIMD loop
  to process the final column `(w + 1) / 2 - 1` when `(w + 1) / 2 % N == 1`, skipping
  boundary reflection and causing scalar/SIMD divergence and potential out-of-bounds
  loads. Guarded the vector loop bound to `half_w - 1 - ((half_w - 2) % N)` (with a minimum of 1),
  ensuring the final column is always safely mirrored by the scalar tail loop.
  Regression test added in `core/test/test_adm_dwt2_x86.c`.
