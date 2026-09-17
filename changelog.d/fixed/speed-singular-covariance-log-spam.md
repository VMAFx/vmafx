- **The SpEED extractors no longer print one warning per solve when the
  covariance matrix is singular.** Flat or linearly-graded chroma has no
  rank-25 covariance, so every solve on such a frame is singular and the notice
  arrived four times a frame per channel — 192 lines for a 48-frame clip, and
  tens of thousands on a real encode, burying every other line of output. The
  condition is an ordinary outcome that each backend already handles by zeroing
  the solution, so it is now reported once when it first happens and once more
  at close with the count: `covariance matrix was singular on N of M solves`.
  Scores are unchanged. Applies to the CPU extractor and to the CUDA, HIP and
  SYCL twins of both `speed_chroma` and `speed_temporal`.
