dev/Containerfile image-size optimization (ADR-0790).

Four targeted layer changes reduce the final image size by several GB with no
runtime behaviour changes:

- `pip install` calls now pass `--no-cache-dir`; downloaded wheels are no longer
  retained in the image layer.
- The `clinfo` apt install is merged into the NEO `.deb` install layer, removing
  one redundant `apt-get update` round-trip and one image layer.
- `/build/ffmpeg` (FFmpeg source tree + compiled object files) is removed in the
  same `RUN` step as `make install`. Installed binaries and libraries under
  `/usr/local/` are unaffected.
- `/build/vmaf/core/build` (Meson build directory including CUDA PTX, SYCL AOT
  fat binaries, HIP `.co` objects) is removed in the same `RUN` step as
  `ninja install`. The installed `libvmaf.so` and headers under `/usr/local/`
  are unaffected.

See [ADR-0790](../docs/adr/0790-containerfile-layer-optimization.md).
