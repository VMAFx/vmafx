- **A score read of a frame still on a worker thread answered "invalid"
  (RC4, ADR-2090).** `vmaf_feature_score_at_index()` waited for the worker
  threads only when the frame's slot existed but was unwritten; for a fed
  frame whose feature had no slot yet (its first score, or a frame past the
  first eight) it returned `-EINVAL` at once. It now waits for the frames in
  flight before it answers, as the documentation of `-EAGAIN` describes; a
  feature name no extractor writes still returns `-EINVAL`.
