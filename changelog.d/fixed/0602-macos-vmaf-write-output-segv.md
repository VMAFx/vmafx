- **macOS SIGSEGV in `vmaf_write_output()` when `pic_cnt == 0` (PR #1355
  follow-up, ADR-0602).** Two tests (`test_write_output_json_path`,
  `test_vmaf_write_output`) crashed on macOS clang with SIGSEGV signal 11
  despite passing valid non-NULL context and path arguments.  Root cause:
  `vmaf->pic_cnt` stays zero when scores are injected via
  `vmaf_import_feature_score` rather than `vmaf_read_pictures`; the
  JSON/XML pooled-metrics helpers then computed `pic_cnt - 1` (unsigned),
  wrapping to `UINT_MAX`, which passed the `index_low > index_high` guard
  in `vmaf_feature_score_pooled` and created a loop that Apple Clang did
  not always exit before a guard-page hit.  Fix: explicit `pic_cnt > 0`
  guards in `json_write_pooled_entry` and `xml_write_one_metric_pools`
  skip pooling when no frames were read; `vmaf_write_output_with_format`
  gains a NULL guard for the `vmaf` context itself and
  `vmaf->feature_collector`; `vmaf_write_output_json` gains the same NULL
  guards already present in `vmaf_write_output_xml`.  Regression test
  `test_write_output_pic_cnt_zero` added to the `fast` suite.
