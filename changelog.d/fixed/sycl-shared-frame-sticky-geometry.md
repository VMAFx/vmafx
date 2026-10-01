- **Shared SYCL frame buffers re-allocate when geometry changes.** When a single
  `VmafSyclState` was shared across consecutive `VmafContext` instances of different
  frame dimensions or bit depths, `vmaf_sycl_shared_frame_init()` kept the old
  buffer allocations and pitch, resulting in out-of-bounds reads and incorrect
  metric scores. Re-initialization now drains queues and reallocates the shared
  frame and chroma buffers to match the new geometry.
