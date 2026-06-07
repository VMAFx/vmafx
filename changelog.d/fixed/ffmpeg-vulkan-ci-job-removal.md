- FFmpeg Integration CI: removed stale `ffmpeg-vulkan` job that failed with
  `ERROR: Unknown option: "enable_vulkan"` on every push to master. The
  Vulkan backend was dropped in ADR-0726 (PR #47), which removed the
  `enable_vulkan` meson option; the CI job was not cleaned up at the same
  time. Workflow renamed from `FFmpeg Integration — Linux/macOS × gcc/clang +
  SYCL + Vulkan` to `FFmpeg Integration — Linux/macOS × gcc/clang + SYCL`.
  Patches 0004 and 0006 remain as no-op compatibility shims per series.txt /
  ADR-0860.
