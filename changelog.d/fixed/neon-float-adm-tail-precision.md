- **NEON `float_adm_csf_den_scale_neon` / `float_adm_sum_cube_neon` —
  scalar tail computed the cube in `double`, the vector body in `float`.**
  The 4-wide NEON body cubes each lane in `float` (`vmulq_f32`) and widens the
  result to `double`, exactly as `adm_csf_den_scale_s` / `adm_sum_cube_s` and
  the AVX2 / AVX-512 twins do. The scalar tail instead did
  `double val = fabs((double)factor * (double)row[j]); accum += val * val * val;`,
  keeping both the `factor` product and the cube in full `double`. For any
  rectangle whose width is not a multiple of four, the last 1–3 columns of every
  row therefore contributed a value the corresponding vector lane would never
  have produced. Tails now use `float val = fabsf(factor * row[j]);
  row_accum += (double)(val * val * val);`. The double lane accumulators are
  also reset per row and folded into the outer accumulator at the end of each
  row, matching `accum_inner_*` in the scalar reference and `row_accum` in both
  x86 twins (the shape `changelog.d/fixed/neon-float-adm-double-accumulator.md`
  already described but the code did not implement). Not user-visible: these
  three kernels still have no dispatch caller (ADR-0873 gap 2), and a pre/post
  `vmaf --feature float_adm --feature adm` run on aarch64 is byte-identical.
  New regression gate: `core/test/test_float_adm_neon.c`.
