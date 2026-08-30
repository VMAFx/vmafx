- **FFmpeg `libvmaf` filter — CUDA backend selector
  ([ADR-0350](../docs/adr/0350-ffmpeg-libvmaf-cuda-backend-selector.md)).**
  Adds `ffmpeg-patches/0010-libvmaf-wire-cuda-backend-selector.patch`,
  giving FFmpeg builds a `cuda` boolean AVOption on the existing
  `libvmaf` filter (default off) plus a user-facing
  `./configure --enable-libvmaf-cuda` flag. With `cuda=1` the filter
  inits a `VmafCudaState` against the CUDA primary context (device
  picked by `CUDA_VISIBLE_DEVICES`, matching the libvmaf CLI `--cuda`
  flag), imports it into the `VmafContext`, and dispenses
  `VmafPicture`s from a `HOST_PINNED` preallocation pool so software
  AVFrame input flows into pinned-host memory the CUDA feature kernels
  DMA from without a staging copy. Mirrors the existing SYCL
  (`0003-libvmaf-wire-sycl-backend-selector.patch`) and Vulkan
  (`0004-libvmaf-wire-vulkan-backend-selector.patch`) selector
  patterns, and coexists with the upstream dedicated `libvmaf_cuda`
  filter — the new selector fires only when the dedicated filter is
  not enabled (`CONFIG_LIBVMAF_CUDA && !CONFIG_LIBVMAF_CUDA_FILTER`).
  Closes the FFmpeg integration gap: `--enable-libvmaf-cuda` is now
  exposed alongside `--enable-libvmaf-sycl` / `--enable-libvmaf-vulkan`
  and configure `--help` advertises the three symmetrically.
  Cumulative replay 0001..0010 against pristine FFmpeg `n8.1.1` PASS.
