## `vif_tools.c`: AVX2 row dispatch taken for a filter with a tap (2026-10-09)

- `vif_filter1d_vertical_dispatch_s()` in `core/src/feature/vif_tools.c` takes
  the AVX2 row pass only for `fwidth >= 1`, the condition under which its row
  table is filled for every entry `convolution_f32_avx_rows_s()` reads.
  cppcheck 2.19.0 reports `uninitvar` on the table without it. **On sync**:
  keep the `fwidth >= 1 &&` in front of `vif_use_avx2_convolution()` when
  upstream changes that dispatch; no score changes with it (the Netflix golden
  gate passes unchanged).
