- `vmaf --threads N` now works on the CUDA and SYCL backends and produces results
  bit-identical to the serial run. It previously failed for every `N`, on every input,
  with `context could not be synchronized` and exit 234 — a misreport: the CUDA context
  was healthy and all four driver calls returned success, while a feature extractor's
  duplicate-write `-EINVAL` was folded into the same error variable. The underlying cause
  was `flush_context_threaded()` flushing temporal GPU extractors that
  `flush_context_cuda()` / `flush_context_sycl()` own, which also emitted the last
  batch-boundary frame without the `min()` that `motion2` / `motion3` are defined by. GPU
  flush errors are now reported distinctly from context-synchronization errors. See
  [ADR-1197](docs/adr/1197-gpu-threaded-flush-ownership.md).
