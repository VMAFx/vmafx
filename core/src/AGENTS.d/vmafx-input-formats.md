---
paths:
  - core/src/vmafx/import_layout.h
  - core/src/vmafx/import_layouts_gen.h
  - core/src/vmafx/import_convert.h
  - core/src/vmafx/rgb_math.h
  - core/src/vmafx/rgb_convert.c
  - core/src/vmafx/rgb_convert.h
  - core/src/vmafx/rgb_coefficients_gen.h
  - core/tools/yuv_input.c
  - core/tools/y4m_input.c
invariant: One format table, one reference per layout, RGB never guessed; the CLI reads raw layouts with the import's code.
---
<!-- markdownlint-disable MD013 -->
# VMAFx input formats and RGB (ADR-2145, ADR-2146)

- `[[pixel_formats]]` in `core/api/vmafx.toml` is the one table: it generates `import_layouts_gen.h` (the rows `frame_import.c` and the CLI read), the FFmpeg / GStreamer lists in `core/api/generated/` and `docs/usage/pixel-formats.md`. A new format = an enum value + a row + a device test per backend listed in `devices`; the generator refuses a mismatch. Never edit a generated file.
- `import_layout.h` is header only (row type, extent, read plan). The library, the CUDA / HIP / SYCL lanes and `core/tools/yuv_input.c` call it; do not copy a layout rule into a second place. V210 is the grouped read form (`period`, `group_elems`, `pattern`) of `VmafxImportRead`; the device gather kernel takes the same three arguments.
- RGB: `rgb_math.h` is the one expression (Q30, int64, one rounding, clip). It is freestanding (no float, no division, no run-time array index: SYCL scratch, ADR-1395) and compiled by the CPU reference, nvcc and hipcc (`VMAFX_RGB_FN`). Coefficients come from `[[color_matrices]]` through `rgb_coefficients.py` (per matrix, range pair and bit depth: the full swing is `2^bpc - 1`). No default matrix: `vmafx_rgb_check_statement()` refuses a missing statement by field name, BT.2020 CL / ICtCp / linear by `VMAFX_E_NOTSUP`.
- A change to `rgb_math.h` or the generator changes `core/test/vmafx_rgb_oracle.h` (`scripts/dev/gen_rgb_oracle.py --write`) and the CUDA / HIP / SYCL kernels in the same PR. FFmpeg fixtures: `scripts/dev/gen_format_fixtures.py --write`.
- CLI: `raw_input_open()` takes a `VmafPixelFormat` (1 to 4) or a `VmafxPixelFormat` layout (from 16); `raw_input_set_rgb()` carries the statement. y4m high-bit-depth tags are table driven (`y4m_setup_hbd_format()`).
- Tests: `test_vmafx_import_ffmpeg_oracle`, `test_vmafx_import_formats_cpu`, `test_vmafx_rgb_convert`, `test_vmafx_rgb_math_contract`, `test_vmaf_raw_layouts`, `scripts/codegen/tests/test_vmafx_api_formats.py`; device: `test_vmafx_import_cuda_formats`, `test_vmafx_import_hip_formats`.
