### Removed

- **Vulkan compute backend removed** (ADR-0715 / Research-0733). The Vulkan backend
  (`core/src/vulkan/`, `core/src/feature/vulkan/`, `libvmaf_vulkan.h`) is removed.
  All Vulkan feature kernels, GLSL shaders, tests, docs, and ffmpeg-patches are
  dropped. Three long-standing Vulkan open bugs are closed by removal:
  T-VK-1.4-BUMP, T-VK-CIEDE-F32-F64, T-VK-VIF-1.4-RESIDUAL-ARC.
  The `--backend` flag no longer accepts `vulkan`. GPU coverage is unaffected —
  every Vulkan-capable device is served by CUDA, SYCL, HIP, or Metal.
  (~30 000 LOC removed; ffmpeg-patches series reduces from 6 to 4 patches.)
