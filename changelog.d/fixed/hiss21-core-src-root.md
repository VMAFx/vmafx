- **HISS-21 burn-down — `core/src` root, `interop/` and `mcp/` (40 findings)**:
  removed every `goto` from the library-runtime lifecycle paths and split the
  oversized lifecycle functions into cohesive `static` helpers, with no change
  to observable behaviour. `picture.c`, `picture_pool.c`, `picture_pool.cpp`,
  `gpu_picture_pool.cpp`, `predict.c` and `read_json_model.c` lose their
  cleanup ladders: each former label became either a guard-clause unwind or a
  named teardown helper that frees exactly the same resources in exactly the
  same order on every exit path. `vmaf_picture_pool_fetch`,
  `vmaf_mcp_start_uds`, `pel_blob_pack`, `pel_blob_find_section`,
  `pel_qp_report_from_blocks` and `pel_x265_csv_parse` are split below the
  60-line Rule-4 budget, and `split_fields` in the x265 CSV reader now states
  its `PEL_CSV_LINE_MAX` scan bound explicitly instead of looping `for (;;)`,
  treating bound exhaustion (a buffer with no terminator) as a rejected line.
  No score changes: the arithmetic in `vmaf_bootstrap_predict_score_at_index`
  and in the QP block-to-cell fold was moved statement-for-statement or left in
  place, so no floating-point expression was re-associated and no accumulation
  order changed.

- **`core/src/interop/pelorus_interop.c` clang-tidy debt actually removed**:
  the function split above traded three `readability-function-size` findings
  for one extra `bugprone-casting-through-void` and one new
  `readability-non-const-parameter`, leaving the file's measured debt at ten —
  no net improvement. `blob_validate_framing` now publishes the header pointer
  it already derived (one cast per constness instead of two),
  `qp_fold_blocks_to_cells` hands its innermost block fold to
  `qp_cell_average` so the remaining size finding clears the nesting threshold,
  and `validate_pack_args` marks the out-parameter it only NULL-checks as
  const. The file measures seven diagnostics, tightened in
  `scripts/ci/tidy-baseline-cpu.json`. Cell values are unchanged: the fold moved
  statement-for-statement with the same `int64_t` accumulator, the same
  row-major traversal and the same truncating division.
