- **HIP devices keep refusing Vulkan images with OPTIMAL tiling
  ([ADR-3353](docs/adr/3353-hip-keeps-refusing-vulkan-optimal.md)).** The
  refusal (naming `desc.vulkan_tiling`) is now a recorded decision, not an open
  gap: ROCm reads the memory of such an image as a linear image while RADV
  tiles it, and the CUDA-style array import faulted on a gfx1036 GPU. Export
  buffers, LINEAR images or linear DRM-modifier images for a HIP device.
