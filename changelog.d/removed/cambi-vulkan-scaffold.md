### Removed

- **CAMBI Vulkan scaffolding** (`core/src/feature/vulkan/cambi_vulkan.c`,
  `shaders/cambi_{preprocess,derivative,filter_mode,decimate,mask_dp}.comp`,
  `core/test/test_cambi_vulkan.c`) removed per ADR-0726 (Vulkan backend
  dropped). Build references in `core/src/vulkan/meson.build` cleaned up.
  Comments in `cambi_internal.h`, `integer_cambi_cuda.c`, and
  `integer_cambi_hip.c` updated to reflect the removal.
