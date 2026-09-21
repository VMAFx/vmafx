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
