- **aarch64 VMAF scores are correct again.** `adm_dwt2_8_neon` computed the
  `j = 0` output column of every DWT2 row from only **three** of the filter's
  **four** taps: the special-case loop read `idx < 3` while the scalar reference
  (`adm_dwt2_8` in `core/src/feature/integer_adm.c`) multiplies all four
  (`filter_lo[0..3]` against `s0..s3` from `ind_x[0..3][j]`). The dropped tap is
  `ind_x[3][0]`, coefficient `-4240`. Effect: `integer_adm2`, `integer_adm3` and
  `integer_adm_scale3` drifted low on ARM, moving the akiyo golden score from
  `88.030463` to `88.030322` and failing three Netflix golden assertions on the
  `Build — Ubuntu ARM clang (CPU)` leg.
- This corrects the diagnosis in [ADR-1057](docs/adr/1057-neon-fma-float-adm-dwt2.md),
  which attributed the drift to **float**-ADM NEON FMA contraction. It is not an
  FP-contraction problem at all: integer ADM accumulates in `int64`, which is why
  PR #1060's `-ffp-contract=off` approach could not have worked, and why building
  the whole tree with `-ffp-contract=off` on aarch64 leaves the score at
  `88.030322`, unchanged. The bug is an integer off-by-one in the tap count.
