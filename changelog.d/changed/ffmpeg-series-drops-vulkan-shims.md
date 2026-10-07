- **The FFmpeg patch series no longer carries the Vulkan shims, and adds no
  compiler warning of its own
  ([ADR-2166](docs/adr/2166-ffmpeg-series-drops-vulkan-shims.md)).** Patches
  `0004` (the `vulkan_device` selector) and `0006` (`libvmaf_vulkan`) were kept
  as no-op shims after the Vulkan backend was removed, only so that the hunks
  of nine later patches replayed; they are gone, with every Vulkan line of
  those nine, 601 patch lines and one FFmpeg configure probe of a header that
  no longer exists. The other patches keep their numbers. The series now adds
  no diagnostic under `-Wextra` on gcc 16.2.1 or clang 23.1.1 beyond FFmpeg's
  own `{NULL}` option-table convention (it added six: four `-Wsign-compare`
  lines of the gcc-hardening patch `0019`, the Vulkan hunk, and one in
  `vf_vmafx.c`), and none in FFmpeg's default warning set. The patch index
  `ffmpeg-patches/README.md` lists `0012`, `0014`, `0017` and `0021` to `0024`,
  which it lacked.
