<!-- markdownlint-disable MD013 MD041 -->
# Research-1182: Windows UTF-8 Path Contract Investigation & Design

## Executive Summary

Netflix/vmaf upstream issue #1568 identified that `libvmaf` fails on Windows when file paths contain non-ASCII
UTF-8 characters (e.g. accented Latin characters, CJK glyphs, special symbols). The defect is rooted in the standard
C runtime `fopen` and `_open` on Windows interpreting narrow `const char *` paths according to the system ANSI code
page (ACP) rather than UTF-8. On POSIX systems, paths are transparent byte strings, making UTF-8 work naturally.

This research establishes the cross-platform contract that all file path parameters passed to `libvmaf` functions
and CLI tools are UTF-8 encoded on all platforms, implemented via an internal compatibility shim.

## Problem Analysis

### 1. Root Cause in Windows CRT

On Windows (MSVC CRT / MinGW), `fopen` and `_open` take `const char *` and convert to UTF-16 using `CP_ACP`.
When the process ANSI code page is Windows-1252 or similar, UTF-8 multibyte sequences:

- Produce file-not-found errors (`ENOENT`),
- Corrupt filenames into mojibake, or
- Fail with permission or path length errors.

Standard POSIX systems (Linux, macOS, BSDs) treat filesystem paths as raw byte sequences, which are transparently
UTF-8 in modern environments.

### 2. Audit of File Open Call Sites

An audit across `core/src/` and `core/tools/` identified all path open sites:

1. `core/src/libvmaf.c:output_file_open` — opens log/score output files (`vmaf_write_output`).
2. `core/src/read_json_model.c` and `core/src/read_json_model.cpp` — model JSON file parsing.
3. `core/src/dnn/model_loader.c` — ONNX model file reading and sidecar discovery.
4. `core/tools/vmaf.cpp` — CLI reference and distorted video inputs, and JSON backend output files.
5. `core/tools/vmaf_bench.c` — YUV test dataset files.
6. `core/tools/vmaf_per_shot.c` — reference video reading and per-shot plan file output.
7. `core/tools/vmaf_roi.c` — reference video reading and ROI sidecar emission.
8. `core/tools/vmaf_vpl.c` — elementary stream input file reading.

*Note on Pelorus Interop*: `core/src/interop/pelorus_qp_report_csv.c` also opens CSV files, but per ADR-1113
it is a verbatim mirror of `VMAFx/pelorus` governed by `scripts/sync-pelorus-interop.sh` and tracked in
`scripts/ci/pelorus-mirror-paths.txt`. It must remain untouched in this repository.

### 3. Symbol Visibility and Architecture

Under ADR-0379 and `check_exported_symbols.py`, `libvmaf.so` strictly enforces default hidden symbol visibility.
Any symbol exported from `libvmaf.so` must be declared with `VMAF_EXPORT` in a public header under `core/include/`.
Exposing `vmaf_fopen_utf8` and `vmaf_open_utf8` in the public ABI would expand the public library surface.
Instead:

- The compatibility shims live in `core/src/compat/path_utf8.{h,c}` with internal visibility.
- `libvmaf` compiles `path_utf8.c` via `dnn_sources`.
- CLI tools (`vmaf`, `vmafx`, `vmaf-perShot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`) and standalone test harnesses
  compile `../src/compat/path_utf8.c` directly.
- Unit tests verify both POSIX pass-through and Windows wide conversion.

### 4. Safety and JPL Coding Standards

The shim conforms to NASA JPL Power of 10 rules:

- **Rule 2 (Bounded Buffers)**: Buffer allocations use strict limits `UTF8_PATH_MAX` (4096) and `UTF8_MODE_MAX` (32).
- **Rule 4 (Short Functions)**: Each function is <= 60 lines of code.
- **Rule 5 (Assertions)**: Non-null preconditions are asserted.

### 5. Verification Strategy

1. **Linux Native**: Meson fast suite unit tests (`test_path_utf8`) test round-trip writing and reading of files with
   accented and CJK characters (`é`, `日`), NULL argument validation, and nonexistent path handling.
2. **Windows Cross-Compilation**: `zig cc -target x86_64-windows` compiles `path_utf8.c` and `test_path_utf8.c` with
   `-Wall -Wextra -Werror`.
3. **Windows Win32 Execution**: The cross-compiled binary runs under Wine, exercising `MultiByteToWideChar`, `_wfopen`,
   `_wopen`, `GetFileAttributesW`, and `EILSEQ` error translation on invalid UTF-8 sequences.
