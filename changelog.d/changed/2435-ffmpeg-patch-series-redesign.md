- **FFmpeg patch series redesigned from 0001, Vulkan shims dropped, and legacy filters retired!**
  The fork's FFmpeg patch stack is restructured into six consecutively-numbered patches (`0001` to `0006`)
  grouped by purpose, completely removing the legacy no-op Vulkan shims (ADR-2166 supersedes ADR-0860) and
  retiring all 19 obsolete patches that touched `vf_libvmaf.c` or added legacy `vmaf`-named filters.
  Fork FFmpeg builds configure solely with `--enable-libvmafx`, activating the unified `vmafx`, `vmafx_tune`,
  and `vmafx_pre` filters on the modern VMAFx C API, while unpatched upstream FFmpeg retains its built-in
  `libvmaf` filter against the `libvmaf.so.3` compatibility layer. Colorimetry forwarding from legacy patch
  `0022` (ADR-2093) is ported to `vf_vmafx.c`, automatically forwarding `AVFrame` color properties to the engine.

  Migration: see [Migrating from the libvmaf filters](docs/usage/ffmpeg.md#migrating-from-the-libvmaf-filters).
