- Fixed Vulkan backend build break under GCC 16 (`-Wreturn-mismatch` is now a hard
  error): `reduce_partials` in `float_ansnr_vulkan.c` and `cambi_vk_readback_image`
  / `cambi_vk_readback_mask` in `cambi_vulkan.c` were declared `static void` but
  contained `return <int-expr>;` guard clauses for `vmaf_vulkan_buffer_invalidate`.
  Changed all three functions to `static int`; call sites now propagate the error
  code instead of proceeding into the readback loop on a failed coherency flush
  (ADR-0376).
