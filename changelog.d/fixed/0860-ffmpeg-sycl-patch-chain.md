- **FFmpeg integration CI — SYCL leg unblocked (#TBD).** Re-include
  `0004-libvmaf-wire-vulkan-backend-selector.patch` and
  `0006-libvmaf-add-libvmaf-vulkan-filter.patch` in
  `ffmpeg-patches/series.txt` as no-op compatibility shims so downstream
  patches (`0005`, `0008`, `0010`–`0015`) — whose hunk context references
  Vulkan-conditional blocks — `git am --3way` cleanly. The Vulkan
  runtime remains gone per ADR-0726; the shims contribute zero linked
  code (`CONFIG_LIBVMAF_VULKAN` evaluates false at FFmpeg configure time
  because `libvmaf_vulkan.h` is absent). See [ADR-0860].

  [ADR-0860]: docs/adr/0860-ffmpeg-patch-chain-no-op-vulkan-shim.md
