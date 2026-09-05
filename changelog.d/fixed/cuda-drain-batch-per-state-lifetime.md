- CUDA: the ADR-0242 drain batch is scoped to the engine that opened it. It was
  thread-local with no owner, so two `VmafContext`s on one OS thread shared it: a
  closed context left its `CUevent`s and its `bool *` drained flags registered and
  the next context waited on freed objects. Registration by a foreign owner is now
  refused, a flush consumes its entries, and `vmaf_close()` clears them.
- `vmaf_score_at_index()` and `vmaf_feature_score_at_index()` no longer read the
  feature collector without a fence (Netflix/vmaf#1305): when the lock-free read
  reports the slot unwritten they wait for the worker threads, drain the CUDA batch
  and run the pending collect for indices at or below the requested one, then read
  again. Reads that hit written slots are unchanged, so the streaming path keeps
  its throughput.
