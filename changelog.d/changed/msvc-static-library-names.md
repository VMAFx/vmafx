- **A static MSVC build installs `vmaf.lib` and `vmafx.lib` (Netflix/vmaf
  `3b4dd350e`).** MSVC, clang-cl and icx-cl builds with
  `--default-library=static` name their installed libraries the way the MSVC
  linker opens `-lvmaf` / `-lvmafx`, instead of Meson's `libvmaf.a` /
  `libvmafx.a`; consumers such as FFmpeg's MSVC toolchain link them without
  renaming ([Library files of an MSVC build](docs/getting-started/building-on-windows.md#library-files-of-an-msvc-build)).
