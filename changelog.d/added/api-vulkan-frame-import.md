- **VMAFx imports Vulkan frames (RC4, ADR-2152).** In a build with the CUDA,
  SYCL or HIP backend, `vmafx_frame_import` takes frames a Vulkan producer
  exported (FFmpeg's Vulkan decode and filters, libplacebo, a GStreamer
  Vulkan element) as `VMAFX_MEMORY_VULKAN`, without a copy through the host
  and without the library creating any Vulkan object. The descriptor names
  how the memory was exported (`vulkan_handle_type`), the images' tiling
  (`vulkan_tiling`), dedicated allocations (`vulkan_flags`) and the
  producer's GPU (`vulkan_pci`; `VmafxDeviceInfo.pci` gives each device's
  PCI location); `acquire_more` carries the fences of a frame written under
  several timelines. A CUDA device reads OPTIMAL images as CUDA arrays and
  LINEAR images and buffers as device pointers, waits on the producer's
  timeline semaphores (`VMAFX_FENCE_VULKAN_SEMAPHORE`) on its stream and
  signals them back behind the last reader (`vmafx_frame_signal_on_release`).
  SYCL and HIP devices read LINEAR and DRM-modifier frames through their
  dma-buf paths, with a sync_file of the producer's write as the acquire
  fence and a host release fence. Imported frames score bit for bit as the
  same frames uploaded from the host. Memory of another GPU, a plane of a
  multi-plane image, Windows handles, OPTIMAL tiling on SYCL and HIP and
  Vulkan semaphores on SYCL and HIP are refused, each naming its field.
  ABI 0.1.10 on the integration branch (0.1.5 on its lane).
  `vmafx_fence_destroy` closes a `VMAFX_FENCE_SYNC_FILE`
  descriptor in every build. See
  [Vulkan frames](docs/api/vmafx/index.md#vulkan-frames).
