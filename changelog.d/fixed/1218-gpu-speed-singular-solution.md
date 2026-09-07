- `speed_temporal` on CUDA, SYCL and HIP now returns `0` when exactly one
  of the reference and distorted temporal differences has a singular
  covariance matrix, matching the CPU rule in `speed_extract_score()`.
  The three temporal twins never reported singularity at all — ADR-1202
  fixed that for the *chroma* twins only — so they returned the score
  kernel's inflated result instead. A static passage makes the reference
  temporal difference identically zero, which is exactly this case.
  Measured on a 960x960 fixture with a frozen reference and a moving
  distorted side: `cpu = 0.00000000` vs `gpu = 230.71379089`, identical
  on all three backends. Re-measure any GPU `speed_temporal` score taken
  over content with static passages.
- All six GPU SpEED twins (`speed_chroma` and `speed_temporal` on CUDA,
  SYCL and HIP) now zero the **device** solution buffer on the singular
  path. They used to `memset` the host staging buffer and upload
  nothing, leaving the device solution at the previous frame's contents
  or, on the first frame, at whatever the allocator returned —
  `sycl::malloc_device` memory is explicitly uninitialised. The host
  `memset` was dead code: the buffer is re-downloaded from the device at
  the top of every pipeline run. See ADR-1218.
