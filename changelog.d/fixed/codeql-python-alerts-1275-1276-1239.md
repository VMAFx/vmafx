- Resolved CodeQL Python alerts 1275, 1276, and 1239 on origin/master by
  implementing active error channel exception diagnostics in `_run_fifo_worker`,
  immediate EOF error channel failure handling in `_fifo_worker_failure`, and
  elementwise identity comparisons for None in `_get_scatter_arrays`.
