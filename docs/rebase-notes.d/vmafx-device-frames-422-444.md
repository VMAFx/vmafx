## Import of 4:2:2 and 4:4:4 layouts (RC4 WP3 formats lane)

`rc4/api-wp3-formats-422-444`, [ADR-2133](adr/2133-vmafx-import-422-444-formats.md),
ABI 0.1.9 (additive; the lane and ADR-2133 say 0.1.5, renumbered when it landed
after the other API additions).

- One CPU reference for every import conversion:
  `core/src/vmafx/import_convert.h` (`vmafx_import_read_plane()`,
  `VmafxImportRead`) with the per-layout plan `vmafx_import_plane_read()` in
  `core/src/vmafx/frame_import.c`. `vmaf_metal_read_plane()`
  (`core/src/metal/iosurface_layout.h`) forwards to it; a rebase that brings
  the Metal row readers back takes this side. `VmafxImportLayout`
  (`internal.h`) gained `packed`, `elem` and `msb`: every row of
  `import_layouts[]` initialises them (no missing-initializer warning).
- Device kernels: `vmafx_import_gather` (its body `vmafx_import_gather_body()`
  in `core/src/vmafx/import_convert_kernels.h`, the entry point in
  `core/src/cuda/import_convert.cu` and `core/src/hip/import_convert.hip`, as
  for the other import kernels), launched per
  output plane from `gather_plane()` of `core/src/cuda/import_frame.c` and
  `core/src/hip/import_frame.c`; the planes of a packed or MSB layout are
  `converted_plane()`; a packed or MSB layout in an array or GL texture is
  refused naming `desc.memory`.
- `vmafx_count_conversion()` counts every converting import
  (`converted_plane(layout, 0)` or `(layout, 1)`), not only interleaved ones.
- Tests: `test_vmafx_import_convert`, `test_vmafx_import_bitexact` (wide
  chroma, other layouts), `test_vmafx_import_{cuda,hip}_formats` with
  `core/test/vmafx_format_cells.h` (shared by every backend lane; the SYCL
  lane's producer plugs in through `VfOps`).
- The SYCL conversion kernels are a separate commit on the SYCL lane
  (`rc4/api-wp3-formats-422-444-sycl`); it carries the same definition
  entries, which this PR lands first: on its rebase they are already there.
- No `libvmaf.h`, golden-data or FFmpeg patch impact.
