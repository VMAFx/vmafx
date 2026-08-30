- Repaired Doxygen drift in three public C API headers
  (`libvmaf_cuda.h`, `libvmaf_vulkan.h`, `libvmaf_sycl.h`):
  added missing `@thread-safety` tags to all five CUDA entry
  points, added `@param` and `@thread-safety` to four Vulkan
  entry points (`vmaf_vulkan_state_init`, `vmaf_vulkan_import_state`,
  `vmaf_vulkan_state_free`, `vmaf_vulkan_list_devices`), clarified
  the `vmaf_cuda_state_free` single-pointer vs double-pointer
  asymmetry (unlike HIP/Metal/SYCL it takes `*` not `**` and does
  not null the caller's handle), and expanded
  `VmafSyclPicturePreallocationMethod` from a two-word stub to a
  fully documented enum matching the CUDA counterpart.
