## VMAFx device frames on CUDA (RC4 WP3 CUDA lane)

`rc4/api-wp3-cuda`, [ADR-2023](adr/2023-vmafx-cuda-device-frames.md),
[ADR-1929](adr/1929-vmafx-device-frames-fences.md).

- New files `core/src/cuda/vmafx_cuda.h`, `vmafx_cuda_internal.h`,
  `import_device.c`, `import_frame.c`, `import_fence.c`, `import_gl.c`,
  `import_pool.c` (in `libvmaf_sources`, not `cuda_static_lib`, because the
  CUDA common objects link into test programs without the VMAFx sources) and
  the kernel `import_convert.cu` (`cuda_cu_sources`, `import_convert_ptx`).
- `core/src/vmafx/*.c` dispatch to the lane under `#ifdef HAVE_CUDA`:
  `device.c` (create, count, info, describe, unref), `device_context.c`
  (engine import; `vmafx_context_release_device()` frees it after a
  successful close), `frame_import.c` (`import_on_device()`, the release
  fence), `fence.c` (CUDA_EVENT and GL_SYNC), `frame_import_admit.c` (the
  per-extractor answer), `frame_pool.c` (CUDA pools), `submit.c` (the planted
  early release). `internal.h` grows `lane` / `lane_release` on
  `VmafxDevice`, `VmafxFrame` and `lane_state` on `VmafxContext`, and exports
  the import layout and plane checks. `vmafx_frame_release()` calls the
  lane's release before the release callback. `register.c` picks the device
  twin when a feature is registered on a device context.
- `core/src/picture.h`: `VmafPicturePrivate.cuda` gains `vmafx` and
  `ordered`. `core/src/libvmaf.c`: `cuda_order_pictures_against_producer()`
  skips the ADR-1199 barrier for a pair of ordered pictures, and
  `translate_picture_device()` refuses a VMAFx frame and counts every other
  download as a host copy. A rebase that touches these keeps both.
- `integer_vif_cuda`: `VifBufferCuda` gains `dis_stride`; both pitches are set
  per frame from the pictures in `vif_submit_scales()`, and
  `vif_vert_load_tiles()` takes both. An upstream sync of `filter1d.cu` must
  keep the second pitch.
- `VmafxFrame.lane_persistent` (`internal.h`): a CUDA pool frame's event
  behind its readers; `vmafx_frame_pool_acquire()` waits on it.
- `float_ms_ssim_cuda` converts level 0 on the device: see
  "`float_ms_ssim_cuda` builds level 0 on the device" (master #2282, carried by
  this stack until its restack).
- Definition: `VMAFX_MEMORY_GL_TEXTURE`, `VMAFX_FENCE_GL_SYNC`,
  `VmafxFrameImport.release` / `user` (ABI 0.1.4); `VMAFX_MIN_FRAME_IMPORT` is
  the 0.1.2 size (`offsetof(VmafxFrameImport, release)`).
