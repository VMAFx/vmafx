- **`libvmaf.pc` advertises the C API version again, so FFmpeg can link the
  fork.** [ADR-1151](docs/adr/1151-vmafx-first-release-1-0-0.md) routed
  `libvmaf.pc`'s `Version:` field to the release-please product version, on the
  reasoning that no consumer could be pinned to the 3.2.1 that master carried
  because no release ever shipped it. The risk was never a pin to an exact
  version — it is the **lower bound** every consumer already compiled in.
  Unpatched upstream FFmpeg's `configure` requires `libvmaf >= 2.0.0`, and the
  fork's own `ffmpeg-patches/` require `libvmaf >= 3.0.0` for the SYCL, Vulkan
  and DNN entry points. Advertising `1.0.0-rc.1` satisfied neither, so the
  first release candidate failed `FFmpeg Ubuntu gcc`, `FFmpeg SYCL`,
  `FFmpeg macOS clang` and `Docker Image Build` with *"Package 'libvmaf' has
  version '1.0.0-rc.1', required version is '>= 2.0.0'"* — and **any**
  unpatched downstream FFmpeg would have refused it the same way. `Version:`
  is now generated from `vmaf_soname_version`, the same number that ships
  `libvmaf.so.3`, so the fork stays a drop-in `libvmaf` while its product line
  starts at 1.0.0. See
  [ADR-1235](docs/adr/1235-pkgconfig-advertises-abi-version.md).
