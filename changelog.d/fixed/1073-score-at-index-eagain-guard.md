- **`vmaf_score_at_index` incorrectly suppressed model prediction for all
  frames after the first in multi-frame sequences.** The `-EAGAIN` guard
  added by ADR-0154 (Netflix#755) was intended to prevent
  `vmaf_predict_score_at_index` from being called before retroactive-write
  input features (e.g. `integer_motion` motion2/motion3) are flushed. The
  guard was correctly placed for the *input* feature case but was also applied
  to the *model output* score slot: after frame 0 prediction creates the
  "vmaf" feature vector (allocating capacity > 1 and writing slot 0), any
  subsequent call to `vmaf_score_at_index` for frames 1+ found slot N with
  `written=false`, which returned `-EAGAIN`; the guard then blocked the
  predict call entirely, propagating `-EAGAIN` through `vmaf_score_pooled`
  for the whole sequence. Fix: change `if (err && err != -EAGAIN)` to
  `if (err)` in `vmaf_score_at_index`. Retroactive-write input features are
  already fully written by the time scoring begins (flush precedes pool), so
  no `-EAGAIN` propagates from the input side. The MCP `compute_vmaf`
  handler's JSON-RPC error on 10-bit multi-frame input is resolved.
  `test_mcp_smoke::test_compute_vmaf_10bit` fixture dimensions bumped 64→192
  as a defensive alignment with the broader test convention. ADR-1073.
