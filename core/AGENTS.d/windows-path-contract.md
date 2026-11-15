---
paths:
  - core/meson.build
  - core/src/meson.build
invariant: Windows internal paths enforce UTF-8 normalization and runtime shims.
area: build-and-toolchains
---
<!-- markdownlint-disable MD013 MD060 -->
# Windows UTF-8 path contract and path shims

- **Windows UTF-8 path contract and internal path shims**
  ([ADR-1182](../../docs/adr/1182-windows-utf8-path-contract.md);
  [Research-1182](../../docs/research/1182-windows-utf8-path-contract.md)):
  `core/src/compat/path_utf8.{h,c}` implements internal UTF-8 open,
  canonicalization, metadata, mkdir, and remove shims. On Windows (`_WIN32`),
  they decode UTF-8 paths to wide strings via `MultiByteToWideChar` and dispatch
  to wide CRT APIs; on POSIX they preserve path bytes. `output_file_open` in
  `core/src/libvmaf.c`, DNN canonicalization/stat gates, CAMBI heatmap directory
  and file creation, and all fork tool/model openers must route through these
  shims. shims are internal and must NOT be exported from `libvmaf.so`
  (no `VMAF_EXPORT`, preserving ADR-0379 ABI stability).
  `core/src/interop/pelorus_qp_report_csv.c` must remain untouched to respect
  ADR-1113 Pelorus verbatim mirror invariant; its narrow path remains open
  until changed in `VMAFx/pelorus` and re-vendored.
