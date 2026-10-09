## VMAFx device frames on SYCL (RC4 WP3 SYCL lane, 2026-10-07)

`rc4/api-wp3-sycl` (on `rc4/api-wp3-cuda`),
[ADR-2091](adr/2091-vmafx-sycl-device-frames.md),
[ADR-1929](adr/1929-vmafx-device-frames-fences.md).

- New files `core/src/sycl/vmafx_sycl.h`, `vmafx_sycl_internal.h`,
  `import_device.c`, `import_frame.c`, `import_dmabuf.c`, `import_fence.c`,
  `import_gl.c`, `import_pool.c` (in `libvmafx_sources` under
  `is_sycl_enabled`, the WP6 split of ADR-2094), the C++ half `vmafx_sycl_rt.{h,cpp}` (`sycl_sources`)
  and `detile.h`. `core/src/vmafx/sync_object.{c,h}` hold the `sync_file`,
  dma-buf implicit-fence and GL sync helpers the CUDA, HIP and SYCL lanes
  share (the CUDA lane's own GL sync loader and the HIP lane's
  `core/src/vmafx/gl_sync.c` and `sync_file.c` moved there; the CUDA and HIP
  imports call it).
- `core/src/vmafx/*.c` dispatch to the SYCL lane under `#ifdef HAVE_SYCL` as
  they do to CUDA; `fence.c` handles `SYNC_FILE` and `GL_SYNC` for every lane;
  `frame_import_admit.c`'s D8 retry also waits on a dma-buf's implicit write
  fences.
- `core/src/picture.h`: `VmafPicturePrivate.sycl` gains `frame`.
  `core/src/sycl/common.cpp`: `sycl_state_create()` (one constructor),
  `vmaf_sycl_state_init_queue()`, and device branches in the shared luma and
  chroma uploads. `core/src/libvmaf.c`: `read_pictures_sycl_device_frame()`.
- Every SYCL twin that stages its own planes has a device branch through
  `vmaf_sycl_picture_read_plane()` (`float_psnr`, `float_motion`,
  `float_adm`, `float_vif`, `float_ssim` / `ssim`, `float_ms_ssim`, `ciede`,
  `ssimulacra2`, the SpEED pair, `motion`'s chroma). An upstream sync that
  touches a twin's staging keeps the branch (see
  `core/src/feature/sycl/AGENTS.d/device-pictures.md`).
- `core/src/sycl/dmabuf_import.cpp`: the Y-tiled and Tile4 de-tile kernels
  are `detile_tiled()` on `detile.h`; `vmaf_sycl_dmabuf_import_queue()` /
  `vmaf_sycl_dmabuf_free_queue()` take any queue's context.
- Definition doc strings only (`VMAFX_MEMORY_GL_TEXTURE`,
  `VmafxDeviceDesc.backend`, the release callback); no ABI change.

## The WP3 SYCL lane on the HIP lanes (2026-10-07)

The WP3 SYCL lane (#2342) lands after the HIP lane (#2341), its GL follow-up
(#2360) and the 4:2:2 / 4:4:4 lane (#2367), as `rc4/integration` merged them.
What a rebase of either side must keep:

- One implementation (HISS-19) of the host-checked producer fences:
  `core/src/vmafx/sync_object.{c,h}` (sync_file poll, dma-buf implicit
  fences, GL sync). The HIP lane's `gl_sync.c` and `sync_file.c` and their
  `internal.h` declarations are gone; HIP code includes `vmafx/sync_object.h`.
  `vmafx_fence_wait()` polls SYNC_FILE and GL_SYNC fences through
  `vmafx_fence_poll()` (the virtual test clock).
- One EGL export: `core/src/vmafx/egl_export.c`. `vmafx_egl_export_planes()`
  takes a `VmafxEglTiled` mode instead of `allow_copy`: the HIP lane passes
  REFUSE or COPY (from `VMAFX_IMPORT_ALLOW_COPY`), the SYCL lane KEEP (a
  tiled export stays tiled for its device de-tile; no writers wait, the
  dma-buf import honours the implicit fences). A target `fourcc` of 0 and a
  NULL `device_pci` skip those checks. `sycl/import_gl.c` keeps only the
  translation into a DMABUF descriptor. This is the fold the HIP GL note
  ("HIP GL textures through EGL dma-bufs") asked for when the lanes met.
  `core/test/test_vmafx_one_egl_export_contract.py` refuses a lane that loads
  EGL itself or a second definition of a sync-object function.
- A SYNC_FILE fence is destroyed by closing its descriptor (ADR-2091 item
  6): `vmafx_fence_destroy()` closes it in every build (on Windows it is
  refused, there are no sync_files), for every lane; the HIP lane's refusal
  (VMAFX_E_NOTSUP) is gone. `test_vmafx_fence_kinds` and
  `test_vmafx_import_sycl`'s sync_file case check it.
- `core/src/sycl/dmabuf_import.cpp` keeps master's descriptor handling
  (`driver_fd()` / `driver_fd_done()`, #2377) inside
  `vmaf_sycl_dmabuf_import_queue()`, which the VMAFx import and the
  VA-surface import share; `test_vmafx_import_sycl`
  `test_import_closes_only_its_own` checks the VMAFx path.
- The HIP GL digest is Research-2161 (both lanes added a Research-2160); the
  SYCL digest keeps 2160.
- No `libvmaf.h`, ABI, golden-data or FFmpeg patch impact.
