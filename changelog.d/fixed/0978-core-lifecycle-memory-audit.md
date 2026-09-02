- `core/src` lifecycle / memory audit (2026-05-31): eight defects
  found and fixed across picture pool, model loader, predict
  pipeline, output writers, dictionary, and feature collector.
  Headline items: data-buffer leak in `pool_preallocate_pictures`
  cleanup when picture allocation partially fails (priv/ref had
  already been detached, so the cleanup `vmaf_picture_unref` was a
  no-op); NULL-version crash in `vmaf_model_load` and
  `vmaf_model_collection_load` (strcmp on a NULL operand);
  `piecewise_segment_apply` and `piecewise_linear_mapping`
  returning bare positive `EINVAL` instead of the project-standard
  `-EINVAL` (silently inverted the sign for any caller that
  propagated `err` upward); `transform()` ignoring
  `piecewise_linear_mapping`'s return value and silently
  overwriting predictions with 0 on knot errors;
  `predict_ensure_caches` leaving `predict_feature_names` in a
  partially-populated state on resolver failure (subsequent calls
  would hit NULL holes); `aggregate_vector_append` returning
  `-EINVAL` on malloc failure where `-ENOMEM` is required; missing
  NULL-fc / NULL-outfile guards in `vmaf_write_output_csv` and
  `vmaf_write_output_sub` (the sibling XML/JSON writers had them
  per ADR-0602); `dict_normalize_numeric` using `strtof` then
  storing into a `double`, silently truncating values with more
  than ~7 decimal digits of precision. New regression tests added
  to `test_predict`, `test_model`, and `test_output`.
