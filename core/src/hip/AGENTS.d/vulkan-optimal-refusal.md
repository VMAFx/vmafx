---
paths:
  - core/src/hip/import_frame.c
  - core/src/hip/import_dmabuf.c
  - core/test/test_vmafx_import_vulkan.c
  - core/test/vmafx_vulkan_test_util.h
invariant: HIP refuses Vulkan OPTIMAL images naming desc.vulkan_tiling; no mapped-mipmap-array route returns.
---
# HIP refuses Vulkan OPTIMAL images

## Rebase-sensitive invariants (ADR-3353)

- A HIP device refuses a `VMAFX_MEMORY_VULKAN` frame with
  `VMAFX_VULKAN_TILING_OPTIMAL`, naming `desc.vulkan_tiling`
  (`core/src/hip/import_frame.c`). Do not add the CUDA route
  (`hipExternalMemoryGetMappedMipmappedArray()` over the OPAQUE_FD memory):
  ROCm describes RADV's tiled memory as a linear image, and the copy out of
  it faulted on gfx1036 (ADR-3353). It returns only with a new ADR backed by
  `test_vmafx_import_vulkan_hip_bitexact` passing over repeated runs.
- `test_vmafx_import_vulkan_hip` asserts the refusal ("OPTIMAL tiling
  refused"); HIP's layout list in `core/test/vmafx_vulkan_test_util.h` has no
  OPTIMAL case.
