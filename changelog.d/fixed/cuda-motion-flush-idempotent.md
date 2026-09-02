- **CUDA `integer_motion` + `float_motion` flush is idempotent on the
  last frame.** T-GPU-OPT-1 (PR #312) introduced a flow in
  `flush_context_cuda` where pending double-buffered work is collected
  via `vmaf_feature_extractor_context_collect` *before* the
  per-extractor `flush_fex_cuda` runs. For both `integer_motion_cuda`
  and `float_motion_cuda`, that pending collect already wrote
  `motion2_score[s->index]` (last frame); `flush_fex_cuda` then
  re-appended at the same index, triggering
  `feature "VMAF_..._motion2_score" cannot be overwritten at index N`
  and propagating `-EINVAL` up to `flush_context_cuda` — surfacing as
  `context could not be synchronized / problem flushing context` even
  though CUDA itself was healthy. Fix: probe
  `vmaf_feature_collector_get_score` before appending; skip the write
  if the slot is already populated. Validated locally on the 576×324
  Netflix golden pair (default model, CUDA score 94.324112 vs CPU
  94.323011, |Δ|=1.1e-3 — within ADR-0214 cross-backend drift envelope).
