# vmafx-node: ffmpeg n8.2 pinned and bundled with ffmpeg-patches

`docker/Dockerfile.node` ships a new `vmafx-node` worker image with four
variants (`node-cpu`, `node-cuda`, `node-rocm`, `node-sycl`).

Each variant includes:
- `ffmpeg` n8.2 compiled from source with all 15 `ffmpeg-patches/` patches
  applied, enabling the `libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`,
  `vmaf_pre`, and `libvmaf_tune` filters.
- Codec inventory: libx264, libx265, libvpx-vp9, libsvtav1, libdav1d.
- `cmd/vmafx-node` Go binary with startup encoder-probe that caches codec
  availability from `ffmpeg -encoders` at launch.

FFmpeg version policy: pinned to the latest stable tagged release (`n8.2`
at time of shipping). Updated on each release-sync PR.

ADR-0717 / Phase 4b.4 (ADR-0709).
