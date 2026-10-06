- **A model collection's score can be read more than once.**
  `vmaf_score_at_index_model_collection()` failed with `-EINVAL` when the
  frame had been scored before, and so did
  `vmaf_score_pooled_model_collection()` over a range holding a frame already
  scored per frame (the collector refused to write the members' scores of
  that frame a second time, `feature "..." cannot be overwritten`). A frame
  already predicted now returns its stored bootstrap scores, as a single
  model's `vmaf_score_at_index()` does; the values are the first
  prediction's, bit for bit.
