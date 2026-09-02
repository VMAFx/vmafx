- Renamed `moment_vulkan.c` → `float_moment_vulkan.c` and `shaders/moment.comp` →
  `shaders/float_moment.comp` to align with the `float_*` naming convention used by all
  other float Vulkan feature variants (`float_motion_vulkan.c`, `float_adm_vulkan.c`, etc.).
  No functional change; `vmaf_fex_float_moment_vulkan` and all four `float_moment_*` metrics
  are unaffected.
