- `psnr_hvs_vulkan`: add `enable_chroma` option (default `true`). When set
  to `false`, only the luma plane is dispatched and only `psnr_hvs_y` is
  emitted; the combined `psnr_hvs` score is suppressed. Mirrors the
  `psnr_vulkan` / ADR-0453 pattern. See ADR-0461.
