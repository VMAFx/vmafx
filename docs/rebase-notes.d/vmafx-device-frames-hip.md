## VMAFx device frames on HIP (RC4 WP3 HIP lane)

`rc4/api-wp3-hip`, [ADR-2092](adr/2092-vmafx-hip-device-frames.md),
[ADR-2023](adr/2023-vmafx-cuda-device-frames.md),
[ADR-1929](adr/1929-vmafx-device-frames-fences.md).

- New files `core/src/hip/vmafx_hip.h`, `vmafx_hip_internal.h`,
  `import_device.c`, `import_frame.c`, `import_dmabuf.c`, `import_fence.c`,
  `import_gl.c` (in `libvmaf_sources` under `if is_hip_enabled`, not in
  `hip_sources`, for the reason the CUDA lane gives) and the kernel module
  `import_convert.hip` (`hip_kernel_sources`, `import_convert_hsaco`).
- Moved out of the CUDA lane into shared files, one copy each:
  `core/src/vmafx/import_convert_kernels.h` (the NV12 / P010 / P016 kernels;
  `cuda/import_convert.cu` and `hip/import_convert.hip` only include it; it
  is in both `cuda_kernel_shared_headers` and `hip_kernel_shared_headers`),
  `core/src/vmafx/gl_sync.c` (the GL loader and `vmafx_gl_sync_acquire()`,
  formerly in `cuda/import_gl.c`), `core/src/vmafx/release_events.c` (the
  release-event table, formerly in `cuda/import_fence.c`), and the new
  `core/src/vmafx/sync_file.c`. A rebase of the CUDA lane that touches the
  old copies applies the change to the shared file.
- `core/src/vmafx/*.c` dispatch to the HIP lane next to the CUDA one
  (`device.c`, `device_context.c`, `frame_import.c`, `fence.c`,
  `frame_import_admit.c`, `submit.c`); `fence.c` waits on `SYNC_FILE` and
  `GL_SYNC` fences without a device; `frame_pool.c` refuses HIP devices.
- `core/src/picture.h`: `VmafPicturePrivate` gains an unconditional
  `hip.str` (one layout in every translation unit; `HAVE_HIP` reaches only
  some through `config.h`). `core/src/libvmaf.c`: `translate_picture()`
  passes a HIP device picture through, and `hip_refuse_other_reader()`
  refuses it to a non-HIP extractor.
- `core/src/hip/picture_hip.c` and `shared_frame.c`: a device picture is
  copied on its library stream and the reader's stream and the null stream
  wait (`vmaf_hip_stream_wait_library()`); an upstream or fork change to the
  upload path keeps the device branch first. `integer_psnr_hvs_hip.c`,
  `ssimulacra2_hip.c` and `integer_ms_ssim_hip.c` (with the new kernel
  `ms_ssim_picture_to_float` in `integer_ms_ssim/ms_ssim_score.hip`) branch
  on `vmaf_hip_picture_device_stream()` before their host staging;
  `core/test/test_vmafx_import_hip_contract.py` holds the order.
- `core/meson_options.txt`: `enable_float_vif_hip_autodispatch` defaults to
  `true`; its description and the `core/src/meson.build` comment keep the
  `ADR-0623` citations `test_stale_text_contract.py` reads.
- Tests: `core/test/vmafx_cuda_cells.h` is now `vmafx_device_cells.h`
  (both lanes read `scripts/ci/exact_twins.d/`); the CUDA tests include the
  new name.
- `core/src/hip/import_frame.c` `fill_failed()`: a GL texture the runtime
  maps but refuses to read (`hipErrorInvalidValue`, every read on ROCm 10.1)
  is `VMAFX_E_NOTSUP` naming `desc.memory`, not `VMAFX_E_DEVICE`;
  `test_vmafx_import_hip_gl` skips on that refusal. Keep both when the GL
  path changes.
- No `libvmaf.h`, ABI (0.1.4 unchanged), golden-data or FFmpeg patch
  impact; `--backend hip` now runs `float_vif_hip` for `float_vif` (the CPU's
  scores, ADR-1444).
