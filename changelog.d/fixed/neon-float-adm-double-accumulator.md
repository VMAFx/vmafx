- **NEON `float_adm_csf_den_scale_neon` and `float_adm_sum_cube_neon` —
  match scalar / AVX2 double-accumulator pattern.** Both NEON kernels
  used a `float32x4_t` lane-vector accumulator and a `float` outer
  accumulator, then reduced with `vaddvq_f32`. On large frames this
  drifts ~10 ULPs from the scalar reference (`adm_tools.c::adm_sum_cube_s`)
  and the AVX2 twin (`float_adm_csf_den_scale_avx2`), both of which
  promote each lane to `double` before accumulation. Fix mirrors the
  AVX2 pattern exactly: per-iteration spill of `val3` via `vst1q_f32`
  into a 16-byte-aligned `float tmp[4]`, lane-wise `(double)`
  promotion, accumulate into `double row_accum`, sum into `double accum`,
  return `(float)accum`. No CI parity gate currently covers this
  path (NEON cross-backend ULP gate is per-feature; ADM-NEON is not
  yet in the matrix), so the fix is silent in CI but closes a
  bit-exactness drift that would surface on the first NEON ADM gate
  added.
