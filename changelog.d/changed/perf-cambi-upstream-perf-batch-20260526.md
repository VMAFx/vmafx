- **CAMBI performance** — ported the upstream feature/cambi optimization
  batch (10 commits): factored 2D SAT into 1D row prefix-sum + column-add,
  refactored `calculate_c_values_row` with an `_avx2` SIMD twin, skip
  histogram updates outside the useful value band, added `filter_mode_avx2`
  and `decimate_avx2`, compact histogram layout (v_band_size × width),
  frame-level `calc_c_values` dispatch, fused sliding-window subtract+add
  into `uh_slide` / `uh_slide_edge`, and moved shared code to `cambi.h`.
  Numerical output preserved (cambi C tests 23/23 pass including the new
  `test_calculate_c_values_scalar_avx2_parity`). Ports upstream commits
  `d655cefe5d`, `9fad7317b7`, `767a6780e8`, `8c60dc9e22`, `bd278ea6d2`,
  `1091b0c190`, `7747425138`, `933cccb4bc`, `984f281f5b`, `41bacc83e1`.
