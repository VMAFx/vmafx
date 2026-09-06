- **CUDA fence batch was keyed by OS thread, so an abandoned
  `VmafContext` poisoned the next one on that thread.** The
  T-GPU-OPT-1 drain batch lived in a
  `static _Thread_local DrainBatchTls g_drain_batch`
  (`core/src/cuda/drain_batch.c`), but every entry in it is a
  `CUevent` and a `bool *` owned by a feature extractor bound to a
  particular `VmafCudaState` — thread scope is strictly wider than
  the handles it stores. The frame loop deliberately returns with
  the batch open and `n > 0` so the next frame can flush it, and
  `vmaf_close()` destroyed the extractor vector (freeing those
  events and flags) while `vmaf_cuda_drain_batch_thread_destroy`
  cleared only the drain *stream*. Any run that abandoned or
  errored out of a CUDA context before the terminal
  `vmaf_read_pictures(NULL, NULL)` therefore left destroyed
  `CUevent`s and dangling `bool *`s in a batch that the next
  `VmafContext` on the same thread flushed — the driver recycles
  event handles, so `cuStreamWaitEvent` usually succeeded against
  an unrelated event and the flush then wrote `true` into freed
  memory; when it did not, `vmaf_read_pictures` failed outright
  with `CUDA_ERROR_INVALID_HANDLE`. Both shapes match the
  intermittent wrong-score / NaN symptom in Netflix/vmaf#1305.
  The batch now lives in `VmafCudaState::drain_batch`
  ([ADR-1187](docs/adr/1187-cuda-drain-batch-state-owned.md)) so it
  cannot outlive the handles it aliases, teardown empties it, and
  `vmaf_close()` runs a best-effort drain **before**
  `feature_extractor_vector_destroy()` so in-flight GPU work has
  landed before the buffers it reads are freed. Bit-exactness is
  untouched: the same events are waited on in the same order, only
  the batch's storage moved. New GPU-gated regression test
  `core/test/test_cuda_drain_batch_state_scope.c`.
- **`vmaf_score_at_index` / `vmaf_feature_score_at_index` /
  `vmaf_score_at_index_model_collection` could return a prediction
  for a frame still in flight.** The GPU path is double-buffered —
  frame N's kernels are collected during frame N+1 — and the three
  per-index readers had no in-flight check, so the model paths fell
  through to `vmaf_predict_score_at_index` over a partially written
  feature row. They now return `-EAGAIN` for an index whose GPU
  work is still pending, a documented "flush and retry" contract
  ([ADR-1189](docs/adr/1189-score-at-index-eagain-contract.md),
  `docs/api/index.md`). Earlier indices and every post-flush read —
  including the whole Netflix golden gate — are unaffected.
- **CUDA compute stream was created at the *lowest* priority.**
  `core/src/cuda/common.c` clamped with `MAX(low, MIN(high, prio))`,
  but CUDA's priority scale is inverted
  (`cuCtxGetStreamPriorityRange` returns
  `(leastPriority, greatestPriority)` with `greatest <= least`), so
  the expression collapsed to `least` — the exact opposite of the
  "use highest priority ... to preempt NVENC/NVDEC" intent stated
  in the comment above it. Measured on an RTX 4090: the stream came
  back at priority `0` (least) instead of `-5` (greatest).
