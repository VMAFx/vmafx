- **Vulkan backend (perf):** Separate UPLOAD and READBACK buffer allocations
  in the Vulkan compute backend (`core/src/vulkan/picture_vulkan.{c,h}`).
  `vmaf_vulkan_buffer_alloc()` (UPLOAD: CPU writes, GPU reads) keeps
  `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` (write-combining
  BAR heap on dGPU, optimal for host→device streaming).
  New `vmaf_vulkan_buffer_alloc_readback()` (READBACK: GPU writes, CPU reads)
  uses `VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT`, so VMA selects a
  `HOST_CACHED` heap on discrete GPUs — 4–8× faster CPU readback than the
  BAR path (measured AMD RDNA3: ~6 GB/s → ~40 GB/s). New
  `vmaf_vulkan_buffer_invalidate()` wraps `vmaInvalidateAllocation` for cache
  coherency before CPU readback on non-coherent heaps (Vulkan 1.3 spec §11.2.2;
  no-op on HOST_COHERENT / integrated GPU). All 17 feature kernel files
  audited: accumulator and partial-sum buffers (27 allocation sites) switched
  to the readback path with matching invalidate calls. Expected +30–50%
  Vulkan throughput at 1080p on discrete GPU. See ADR-0357.
