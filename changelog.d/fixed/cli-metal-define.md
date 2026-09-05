- Fix `vmaf --backend metal` / `--metal_device` / `--no_metal` on macOS.
  `core/tools/meson.build` previously defined `-DHAVE_CUDA=1`, `-DHAVE_SYCL=1`,
  and `-DHAVE_HIP=1` for `vmaf_tool_cflags` but omitted Metal entirely, leaving
  every `#ifdef HAVE_METAL` block in `core/tools/vmaf.cpp` uncompiled and dead.
  The CLI on macOS now receives `-DHAVE_METAL=1` and links `metal_deps` when
  Metal is enabled (`is_metal_enabled`), making `--backend metal`, `--metal_device <N>`,
  and `--no_metal` functional at runtime.
