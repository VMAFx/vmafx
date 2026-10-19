- **The GPU container images, the tester images and the Linux release download
  carry `libvmafx.so.1` next to `libvmaf.so.3` (ADR-2094).** Since the library
  split the `vmaf` CLI and the compat `libvmaf.so.3` both load the VMAFx engine
  library, but these builds copied only the `libvmaf.so*` files: their library
  checks refused the result, or the CLI could not start. The release download
  now has six library files; see
  [Release download](docs/getting-started/index.md#release-download).
