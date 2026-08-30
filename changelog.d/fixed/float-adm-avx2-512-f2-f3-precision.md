## Fixed

- `float_adm_csf_den_scale_avx2/512` and `float_adm_sum_cube_avx2/512`: replaced
  store-to-temp + scalar-accumulate loop with `_mm256_cvtps_pd` / `hsum_ps_to_double`
  widening to remove float-precision intermediate stores (F2 fix, ADR-0844).
- `float_adm_avx2.c` and `float_adm_avx512.c` now compile in isolated per-TU static
  libraries with `-ffp-contract=off`, preventing compiler auto-FMA from diverging
  the DWT2 vertical-pass SIMD chains from the scalar reference (F3 fix, ADR-0844).
