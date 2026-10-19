---
paths:
  - core/src/feature/cambi.c
  - core/src/feature/cambi.h
  - core/test/test_cambi_full_ref_wide_source.c
  - core/test/test_cambi_heatmap_writers.c
invariant: CAMBI bounded searches, c-values window boundaries, row-by-row 10-bit copies, and UTF-8 heatmap paths.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CAMBI Searches, Window Boundaries, and Heatmap Paths

- **CAMBI bounded searches and live private helpers** (ADR-0205 / ADR-1146):
  `cambi.c` is strict-clean: its one suppression is file-scoped
  `NOLINTBEGIN(modernize-use-nullptr)` bracket that ADR-1138 gives every C
  translation unit (it spells null pointer `NULL`, as upstream does;
  `nullptr` on any non-comment line fails `scripts/dev/preflight.sh --stage
  msvcism`), and it has no other `NOLINT` and no Cppcheck suppression.
  Preserve 16-step TVI bisection, `UINT16_MAX`-bounded VLT scan, and
  `n`/partition-span bounds on quick-select without changing comparison,
  pivot, swap, or accumulation order. shared extractor callback ABI stays
  mutable; `read_only_picture_view()` is const-view adapter Cppcheck can
  verify. All ten helpers declared in `cambi_internal.h` must remain exercised
  by real CPU/reference paths as well as available to GPU twins; do not replace
  those calls with analyzer annotations. Keep compact `CAMBI_OPTION`
  descriptors equivalent to public option table. See
  [measured source and binary equivalence](../../../../docs/research/2043-cambi-production-lint-2026-09-08.md).
- **CAMBI c-values walks stay inside short and narrow frames**
  (Netflix/vmaf#1628, port of Netflix/vmaf#1629, plus column bound):
  `calculate_c_values()` in `cambi.c`, `calculate_c_values_avx2()` in
  `x86/cambi_avx2.c` and `cambi_calculate_c_values_frame()` in
  `cambi_c_values_frame.h` (AVX2 scan, AVX-512, NEON) bound first pass to
  `MIN(pad_size, height)` rows, top edge to `MIN(pad_size + 1, height)` and
  start bottom edge at `MAX(height - pad_size, 0)`. scalar and
  AVX2-mirror walks also bound every first column loop to
  `MIN(pad_size, width)`; shared SIMD walk only visits columns below
  `width`. CAMBI decimates in place, so columns past `width` hold finer
  scale's pixels, not zeros, and reading them changes score without any
  sanitizer noticing. Keep three walks identical; upstream sync that
  re-imports `calculate_c_values()` keeps both bounds (upstream has neither
  column bound nor, until #1629 merges, row bounds).
  `test_calculate_c_values_short_frame` and
  `test_calculate_c_values_narrow_frame` (`core/test/test_cambi.c`) fail on
  every driver that loses one. Metal twin runs this walk on host
  through `vmaf_cambi_calculate_c_values()`. SYCL, CUDA and HIP twins
  compute c-values on device and do not call it; `cambi_hip` clips
  each window to frame itself (`cambi_hd_cvals_begin()`,
  `hip/integer_cambi/cambi_hip_device.h`) and must keep matching these bounds.
- **CAMBI copies same-size 10-bit plane row by row**
  (`T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05`):
  `decimate_same_size_16b()` in `cambi.c` copies one row at time with
  input's stride and working picture's stride. Under `full_ref`
  working pictures are allocated `MAX(src, enc)` wide, so with source larger
  than picture their stride exceeds input's; upstream's single
  `memcpy` of `stride * height` samples shifts every row there, and `cambi`
  must not depend on `full_ref` or source size. Do not bring single
  copy back, here or in twin that converts on host (`cambi_metal` calls
  `vmaf_cambi_preprocessing()`). `core/test/test_cambi_full_ref_wide_source.c`
  fails on it, and CAMBI case with `cpu_opts` in
  `core/test/test_{cuda,sycl,hip}_exact_twins.c` holds twin's `cambi`
  equal to CPU's under those options.
- **CAMBI heatmap writers are shared with Metal twin**
  (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05): `open_heatmaps()` takes
  path, encode size and file array instead of `CambiState`, and
  `close_heatmap_files()` is one close loop. `cambi_internal.h` exports
  them with `dump_c_values()` as `vmaf_cambi_open_heatmaps()`,
  `vmaf_cambi_dump_c_values()` and `vmaf_cambi_close_heatmaps()`; `init()`,
  `cambi_score()` and `close_cambi()` call those trampolines, and
  `integer_cambi_metal.mm` calls same three, so its `.gray` files are
  CPU's byte for byte. upstream sync that touches `open_heatmaps()`,
  `dump_c_values()` or close loop keeps state-free signatures and
  file naming, scaling and frame offsets; `core/test/test_cambi_heatmap_writers.c`
  and `REFERENCE_LINES` of
  `test_metal_twin_option_tables_contract.py` fail otherwise. `close_cambi()`
  returns `-EIO` when heatmap file fails to close (its buffered rows are lost).
- **CAMBI heatmap paths are UTF-8 on Windows** (ADR-1182):
  `mkdirp.cpp` must create each component through `vmaf_mkdir_utf8`, and
  `cambi.c::open_heatmaps` must open every `.gray` file through
  `vmaf_open_utf8`. Keep `test_open_heatmaps_utf8_path` as production-seam
  regression; helper-only path test does not protect this call-site wiring.
- **Rust twin `cambi_rust` mirrors `cambi.c`** (RC4,
  [ADR-1713](../../../../docs/adr/1713-rc4-rust-extractor-framework.md)):
  `core/src/rust/feature/cambi/` ports scalar path of `cambi.c`, `cambi.h`
  and `luminance_tools.cpp` statement by statement; must return C extractor
  bits. Change to option table, init post-processing, preprocessing, spatial
  mask, mode filter, c-values walk or quick-select pooling changes matching
  Rust module in same PR; `reciprocal_lut` copied into `lut.rs` as literal
  text, never recomputed. `core/test/test_rust_cambi_kernels.c` and
  `scripts/ci/rust_twin_diff.py --feature cambi` fail when both drift.
