### fix(neon): float_adm dwt2 1-ULP bit-exactness failure from PR #685

Replace `vmlaq_laneq_f32` (FMLA — single-rounding) with explicit
`vmulq_laneq_f32` + `vaddq_f32` (two-rounding, matching scalar reference)
in the `float_adm_dwt2_neon` vertical-pass vectorized loop. Also rewrite
the scalar tail to the sequential `accum += f[N]*sN` pattern from
`adm_dwt2_s` for strict addition-order parity. Closes the regression
introduced by PR #685 (NEON dispatch wiring). ADR-1055.
