- **`vmaf_read_json_model_collection` rejects sub-model name truncation with `-EINVAL`.**
  Port of the sub-model name truncation check from upstream Netflix/vmaf commit
  `15f1447c6` ([Netflix/vmaf#1428](https://github.com/Netflix/vmaf/pull/1428)).
  In both `core/src/read_json_model.cpp` and `core/src/read_json_model.c`,
  the return value of `snprintf` when formatting generated sub-model names
  `"%s_%04u"` was ignored via `(void)snprintf`. When a model collection
  reaches index 9999, `++i` increments to 10000 (5 digits), exceeding
  `cfg_name_sz` (`strlen(name) + 5 + 1`) and truncating the sub-model name.
  Both the C++23 parser and its C twin now validate `n < 0 || (size_t)n >= cfg_name_sz`,
  tear down any allocated model and collection objects without leaking, and
  return `-EINVAL`.
