## Rust `cambi` twin follows `cambi.c` (RC4, ADR-1713)

- `core/src/rust/feature/cambi/` is a statement-by-statement port of the scalar
  path of `core/src/feature/cambi.c`, `cambi.h` (`reciprocal_lut`, the
  `update_histogram_*` / `uh_slide*` helpers) and `luminance_tools.cpp`, and must
  return the C extractor's bits. An upstream sync that changes any of them
  (option table, init post-processing, preprocessing, spatial mask, mode filter,
  c-values, quick-select pooling, EOTFs) changes the matching Rust module in the
  same PR; `reciprocal_lut` is copied as literal text, never recomputed.
  `core/test/test_rust_cambi_kernels.c` (suite `rust`) holds the table, the
  TVI / visibility tables, the adjusted window, the mask index and the resize
  walk to the C functions; `scripts/ci/rust_twin_diff.py --feature cambi`
  re-checks the scores. No public C API or FFmpeg patch impact.
