- **FFmpeg HIP backend selector — `hip_device` AVOption and
  `--enable-libvmaf-hip` configure flag
  ([ADR-0380](../docs/adr/0380-ffmpeg-patches-hip-backend-selector.md),
  patch `0011-libvmaf-wire-hip-backend-selector.patch`).**
  Adds a `hip_device` integer option to FFmpeg's `libvmaf` filter that
  routes VMAF computation through the libvmaf HIP backend when set to
  a non-negative device index. Completes the GPU backend selector
  symmetry on the regular `libvmaf` filter: SYCL (`sycl_device`,
  patch 0003), Vulkan (`vulkan_device`, patch 0004), CUDA (`cuda`
  boolean, patch 0010), and now HIP (`hip_device`, patch 0011) all
  have named selectors accessible without swapping to a dedicated filter.
  The `--enable-libvmaf-hip` configure flag promotes `libvmaf_hip`
  into `EXTERNAL_LIBRARY_LIST` alongside `--enable-libvmaf-sycl` /
  `--enable-libvmaf-vulkan` / `--enable-libvmaf-cuda`. The patch also
  adds an auto-detect `check_pkg_config` line so that when `libvmaf`
  was built with `-Denable_hip=true` but `--enable-libvmaf-hip` was not
  explicitly passed to FFmpeg's `configure`, the `CONFIG_LIBVMAF_HIP`
  symbol is still set and the `hip_device` option compiles in.
  A dedicated `libvmaf_hip` filter for zero-copy ROCm hwdec import
  (analogous to `libvmaf_sycl` / `libvmaf_vulkan`) is deferred until
  FFmpeg exposes a ROCm/HIP hardware-frame context; the gap is tracked
  in `docs/state.md` (row `T-FFMPEG-HIP-FILTER-DEFERRED`).
  CLAUDE.md §12 r14 compliance: `libvmaf_hip.h` C-API surfaces
  (`vmaf_hip_state_init`, `vmaf_hip_import_state`, `vmaf_hip_state_free`)
  are now tracked in the FFmpeg patch stack.
