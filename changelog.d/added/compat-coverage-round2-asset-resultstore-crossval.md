- **`compat/python-vmaf/tests/test_asset.py`** (75 tests): covers `Asset` and
  `NorefAsset` — path generation, YUV type handling, dimension properties,
  start/end frame logic, duration computation, repr/hash/equality, filter
  command accessors, copy(), workfile/procfile paths, and bitrate helpers.
- **`compat/python-vmaf/tests/test_result_store.py`** (32 tests): covers
  `FileSystemResultStore` save/load/delete/workfile round-trips,
  `_to_python_natives` numpy-scalar coercion, and static `save_result` /
  `load_result` helpers.
- **`compat/python-vmaf/tests/test_cross_validation.py`** (33 tests): covers
  `ModelCrossValidation.run_cross_validation`, integer and list-based k-fold
  splitters (LOSO-style), `_find_most_frequent_dict`, `_sample_model_param_list`,
  grid/random search assertors, `format_stats`, and `print_output`.
- **`compat/python-vmaf/tests/test_tools_misc.py`** (61 tests): covers
  `tools/misc.py` pure-Python utilities (path helpers, dict helpers,
  `map_yuv_type_to_bitdepth`, `indices`, `unroll_dict_of_lists`, `neg_if_even`)
  and `tools/decorator.py` utilities (`deprecated`, `dummy`, `memoized`,
  `override`, `persist`). Coverage round 2; brings the `compat/vmaf/tests/`
  suite from 175 to 376 tests.
