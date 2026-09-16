- **Race between the accumulator zeroing and the kernel that reads it
  (CUDA and HIP).** `vmaf_cuda_kernel_submit_pre_launch()` issued
  `cuMemsetD8Async` on `lc->str` — the extractor's private readback
  stream — while the accumulating kernel launched on `picture_stream`.
  CUDA orders work only within a stream, and nothing ordered these two,
  so the memset could land *after* atomic adds had already run and erase
  them. The feature then reported a sum that was too low: the parity
  tests saw `cpu=127.50000000 cuda=120.87500000`. It surfaced only on a
  loaded GPU and never standalone, so it read as a flaky test rather
  than as the race it is — `test_cuda_float_moment_parity*` failed
  about one full-suite run in three. Measured: **10/10 clean full-suite
  runs with the fix, 3/5 without**, same machine and parallelism.
  The zeroing now goes on the same stream the kernel launches on, so
  ordinary program order within one stream does the work. `lc->drained`
  is also cleared at submit: it lets `collect()` skip its
  `cuStreamSynchronize`, is set by a batch flush and cleared only by a
  matching `collect()`, and `vmaf_cuda_drain_batch_close()` clears the
  batch table but not the per-entry flags — so a flag could outlive its
  frame and let a later collect skip a sync it still needed.
  The HIP twin carried the identical defect (`hipMemsetAsync` on
  `lc->str`) and is fixed the same way, by inspection of the CUDA
  original rather than by reproduction.
