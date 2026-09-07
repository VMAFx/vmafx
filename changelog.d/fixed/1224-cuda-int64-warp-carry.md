- Fixed a dropped carry in the CUDA integer-SSIM weight reduction.
  `integer_ssim_score.cu` split the `int64_t` per-pixel weight into two
  32-bit halves and summed each **independently** across the warp,
  recombining only at the end — so a carry out of the low half was lost,
  and `lo` itself could overflow a signed `int32` (undefined behaviour).
  Not reachable today (the 9-tap weight sum stays far below 2^31), but
  wrong in a way that would only appear at large frame sizes or a future
  weight scaling. Replaced with the existing `warp_reduce(int64_t)`
  helper in `cuda_helper.cuh`, which reassembles the halves before
  adding. See ADR-1224.
