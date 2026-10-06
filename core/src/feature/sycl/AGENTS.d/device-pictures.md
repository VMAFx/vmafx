---
paths:
  - core/src/feature/sycl/float_adm_sycl.cpp
  - core/src/feature/sycl/float_motion_sycl.cpp
  - core/src/feature/sycl/float_psnr_sycl.cpp
  - core/src/feature/sycl/float_vif_sycl.cpp
  - core/src/feature/sycl/integer_ciede_sycl.cpp
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/src/feature/sycl/integer_ms_ssim_sycl.cpp
  - core/src/feature/sycl/integer_ssim_sycl.cpp
  - core/src/feature/sycl/speed_chroma_sycl.cpp
  - core/src/feature/sycl/speed_sycl_pipeline.cpp
  - core/src/feature/sycl/speed_sycl_pipeline.h
  - core/src/feature/sycl/speed_temporal_sycl.cpp
  - core/src/feature/sycl/ssimulacra2_sycl.cpp
  - core/test/vmafx_sycl_cells.h
invariant: Twins copy SYCL device pictures on the device via vmaf_sycl_picture_read_plane, never the host (ADR-2091).
---
<!-- markdownlint-disable MD013 MD060 -->
# SYCL device pictures (VMAFx imports)

- **Device pictures are read on the device** ([ADR-2091](../../../../docs/adr/2091-vmafx-sycl-device-frames.md)).
  A frame imported through the VMAFx API reaches `submit()` as a picture of
  buffer type `VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE` whose `data[]` are
  device USM (or a dma-buf mapping) of the device's SYCL context. Every twin
  that stages its own planes tests `vmaf_sycl_picture_on_device()` and copies
  the plane with `vmaf_sycl_picture_read_plane()` on its own queue (SpEED:
  `speed_sycl::read_device_plane()`): the copy waits on the frame's ready
  event and is recorded as a reader, so the frame's release waits on it. A
  host `memcpy()` from such a picture reads device memory on the host and
  crashes or reads garbage; a device copy that bypasses the helper is not
  recorded and the producer may overwrite the plane while it runs.
  `float_ms_ssim` builds level 0 on the device (`launch_picture_to_float`,
  `picture_copy()`'s division by a power of two). **On rebase / change**: a
  twin that starts reading `ref_pic` / `dist_pic` in a new place adds the
  device branch in the same PR; a new exact SYCL twin gets a row in
  `core/test/vmafx_sycl_cells.h` (`test_vmafx_import_sycl_cells_contract.py`
  fails otherwise). Upload failures of the device branch return `-EIO` from
  `submit()`. Guards: `test_vmafx_import_sycl_bitexact` (every exact cell,
  imported == host-uploaded), the parity tests (host branch unchanged).
