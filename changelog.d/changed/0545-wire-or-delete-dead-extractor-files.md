- chore(feature): wire-or-delete dead Vulkan/Metal extractor source files
  (~3500 LOC removed). Wires `float_ms_ssim_metal.mm` + `float_ms_ssim.metal`
  into `core/src/metal/meson.build` to honour ADR-0490's accepted Metal
  port (was missing meson entry — latent link-time bomb on macOS Metal builds
  via `feature_extractor_list[]`). Deletes 7 Vulkan `.c` files (duplicates of
  wired siblings such as `vif_vulkan.c` / `moment_vulkan.c`, or abandoned WIP
  scaffolds), 11 Metal `.mm` files + 11 paired `.metal` kernels, 7 orphan
  Vulkan `.comp` shaders, and one dead `extern vmaf_fex_integer_adm_metal`
  declaration. The retained `adm_vulkan.c` legacy shim (ADR-0468) is out of
  scope and unchanged. See [ADR-0545](../docs/adr/0545-wire-or-delete-dead-extractor-files.md).
