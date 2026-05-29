### Removed

- **Vulkan compute backend removed** (ADR-0726). Deleted
  `core/src/vulkan/`, `core/src/feature/vulkan/`, and public header
  `core/include/libvmaf/libvmaf_vulkan.h`. The `enable_vulkan` meson
  option, FFmpeg patches 0004 and 0006, all Vulkan CI jobs
  (`vulkan-vif-cross-backend`, `vulkan-parity-matrix-gate`,
  `ffmpeg-vulkan`), the volk and VMA subproject wraps, and the
  `docs/backends/vulkan/` documentation tree are removed in the same
  change. CUDA and SYCL remain the active GPU backends.
