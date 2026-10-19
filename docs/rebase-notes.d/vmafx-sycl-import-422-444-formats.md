## 4:2:2 and 4:4:4 import on SYCL (RC4 WP3 formats lane, 2026-10-07)

`rc4/api-wp3-formats-422-444-sycl` (on `rc4/api-wp3-sycl`),
[ADR-2133](adr/2133-vmafx-import-422-444-formats.md).

- `core/src/sycl/vmafx_sycl_rt.{h,cpp}`: `VmafxSyclPlaneOp` gained `step`,
  `offset`, `in_bytes`, `mask` (zero for the other operations) and
  `vmafx_sycl_rt_frame_gather()` / `launch_gather()` read a packed or MSB
  plane by the plan of `vmafx_import_plane_read()`.
  `core/src/sycl/import_frame.c`: `converted_plane()`, `gathered()`,
  `gather_plane()`; the conversion counter counts every converting import.
  A rebase keeps all three; the CPU reference
  (`core/src/vmafx/import_convert.h`) and the CUDA / HIP kernels
  (`import_convert_kernels.h`) are the same plan.
- `core/test/vmafx_sycl_test_util.h`: `vs_layout()` / `vs_write()` go through
  `vt_device_layout()` / `vt_producer_bytes()` (planar, semi-planar and
  packed); `test_vmafx_import_sycl_formats` plugs the SYCL producer into
  `VfOps` of `vmafx_format_cells.h` with `VF_CELLS_HEADER` set to
  `vmafx_sycl_cells.h`.
- The common part of ADR-2133 (pixel formats, ABI 0.1.9, CPU reference,
  CUDA and HIP kernels) landed with #2367; this lane adds the SYCL half only
  and no API entry.
- No `libvmaf.h`, golden-data or FFmpeg patch impact.
