- **SYCL devices wait on and signal Vulkan timeline semaphores on the GPU
  (ADR-3352).** On Linux, `vmafx_frame_import()` of a Vulkan frame on a SYCL
  device takes `VMAFX_FENCE_VULKAN_SEMAPHORE` timelines in `acquire` and
  `acquire_more` and waits on them on the library queue, and
  `vmafx_frame_signal_on_release()` signals the producer's timeline behind the
  frame's last reader, as CUDA devices do. Before, a SYCL device refused
  Vulkan semaphores and the producer had to hand over a `sync_file` and wait
  for a host release fence. See
  [Vulkan frames](docs/api/vmafx/index.md#vulkan-frames).
