- **changed(core):** Route fork-added diagnostic messages through
  `vmaf_log()` instead of raw `fprintf(stderr, ...)`. Direct stderr writes
  bypassed user-installed log callbacks and the `vmaf_set_log_level()`
  filter. Touched sites: 5 in `core/src/libvmaf.c` (`vmaf_write_output`
  guard messages — now `VMAF_LOG_LEVEL_ERROR`), 1 in
  `core/src/sycl/dispatch_strategy.cpp` (`VMAF_SYCL_NO_GRAPH` deprecation
  warning — now `VMAF_LOG_LEVEL_WARNING`), and 5 in
  `core/src/sycl/common.cpp` (device-enumeration failure, graph-submit
  exception handlers, debug upload timing — `VMAF_LOG_LEVEL_ERROR` /
  `VMAF_LOG_LEVEL_DEBUG` as appropriate). User-facing CLI-style stdout
  prints (`vmaf_sycl_list_devices`, `vmaf_sycl_print_timing`,
  `vmaf_sycl_profiling_print`) and `core/src/log.{c,cpp}` (the log
  implementation itself) are intentionally left untouched. Vendored
  `core/src/svm.cpp` (libsvm) deferred to a follow-up — rerouting libsvm
  warnings through `vmaf_log` would change semantics for callers that
  embed libsvm independently. No score / output / ABI change.
