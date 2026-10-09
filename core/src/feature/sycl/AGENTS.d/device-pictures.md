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

- **Device pictures read on device** ([ADR-2091](../../../../docs/adr/2091-vmafx-sycl-device-frames.md)).
  Frame imported through VMAFx API reaches `submit()` as picture of buffer
  type `VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE`; `data[]` = device USM (or
  dma-buf mapping) of device's SYCL context. Every twin staging own planes
  tests `vmaf_sycl_picture_on_device()`, copies plane with
  `vmaf_sycl_picture_read_plane()` on own queue (SpEED:
  `speed_sycl::read_device_plane()`): copy waits on frame's ready event, is
  recorded as reader -> frame release waits on it. Host `memcpy()` from such
  picture reads device memory on host: crash or garbage. Device copy
  bypassing helper not recorded -> producer may overwrite plane while it runs.
  `float_ms_ssim` builds level 0 on device (`launch_picture_to_float`,
  `picture_copy()`'s division by power of two).
- **On rebase / change**: twin reading `ref_pic` / `dist_pic` in new place
  adds device branch in same PR; new exact SYCL twin -> row in
  `core/test/vmafx_sycl_cells.h` (`test_vmafx_import_sycl_cells_contract.py`
  fails otherwise). Upload failure of device branch -> `-EIO` from
  `submit()`.
- Guards: `test_vmafx_import_sycl_bitexact` (every exact cell, imported ==
  host-uploaded), parity tests (host branch unchanged).
