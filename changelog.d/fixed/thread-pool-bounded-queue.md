- Bound pending CPU thread-pool jobs to the created worker count, restoring
  backpressure when decoding outpaces feature extraction. Preserve recycled
  payloads and batch errors, and wait for blocked producers during shutdown.
  Adapted from Netflix/vmaf commit `8fc71e3` with shutdown-lifetime regression
  coverage.
