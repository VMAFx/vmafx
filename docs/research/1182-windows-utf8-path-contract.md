<!-- markdownlint-disable MD013 MD041 -->
# Research-1182: Windows UTF-8 Path Contract Investigation & Design

## Executive Summary

Netflix/vmaf upstream issue #1568 identified that `libvmaf` fails on Windows when file paths contain non-ASCII
UTF-8 characters (e.g. accented Latin characters, CJK glyphs, special symbols). The defect is rooted in the standard
C runtime `fopen` and `_open` on Windows interpreting narrow `const char *` paths according to the system ANSI code
page (ACP) rather than UTF-8. On POSIX systems, paths are transparent byte strings, making UTF-8 work naturally.

This research establishes the cross-platform contract that VMAFx-owned file path parameters passed to `libvmaf`
functions and CLI tools are UTF-8 encoded on all platforms, implemented via an internal compatibility shim.
The vendored Pelorus parser is recorded as an explicit residual rather than silently included in that claim.

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
9. `core/src/feature/cambi.c` + `core/src/feature/mkdirp.cpp` — documented CAMBI heatmap directory and files.

The DNN path has more than an opener: `_fullpath` and `stat` ran before `_wfopen`, so merely widening the final
open still rejected a non-ASCII ONNX path. The benchmark had the same narrow `_fullpath` preflight. A complete
fix therefore also needs UTF-8-aware canonicalization, metadata, directory creation, and test cleanup.

*Note on Pelorus Interop*: `core/src/interop/pelorus_qp_report_csv.c` also opens CSV files, but per ADR-1113
it is a verbatim mirror of `VMAFx/pelorus` governed by `scripts/sync-pelorus-interop.sh` and tracked in
`scripts/ci/pelorus-mirror-paths.txt`. It must remain untouched in this repository. Because the pinned Pelorus
source still calls narrow `fopen`, `pel_x265_csv_parse()` is not yet covered by the contract and the original
12-site backlog row cannot close. The fix must land in `VMAFx/pelorus` first and then be re-vendored.

### 3. Symbol Visibility and Architecture

Under ADR-0379 and `check_exported_symbols.py`, `libvmaf.so` strictly enforces default hidden symbol visibility.
Any symbol exported from `libvmaf.so` must be declared with `VMAF_EXPORT` in a public header under `core/include/`.
Exposing the UTF-8 compatibility helpers in the public ABI would expand the public library surface.
Instead:

- The compatibility shims live in `core/src/compat/path_utf8.{h,c}` with internal visibility.
- `libvmaf` compiles `path_utf8.c` via `dnn_sources`.
- CLI tools (`vmaf`, `vmafx`, `vmaf-perShot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`) and standalone test harnesses
  compile `../src/compat/path_utf8.c` directly.
- Unit tests verify both POSIX pass-through and Windows wide conversion. Production-seam tests separately call
  `vmaf_write_output()`, `vmaf_dnn_validate_onnx()`, `vmaf_dnn_sidecar_load()`, and CAMBI `open_heatmaps()` with
  exact non-ASCII filenames so reverting only the real call-site wiring cannot leave helper-only tests green.

### 4. Safety and JPL Coding Standards

The shim conforms to NASA JPL Power of 10 rules:

- **Rule 2 (Bounded Buffers)**: Buffer allocations use strict limits `UTF8_PATH_MAX` (4096) and `UTF8_MODE_MAX` (32).
- **Rule 4 (Short Functions)**: Each function is <= 60 lines of code.
- **Rule 5 (Assertions)**: Non-null preconditions are asserted.

### 5. Verification Strategy

1. **Linux Native**: Meson fast suite unit tests (`test_path_utf8`) test round-trip writing and reading of files with
   accented and CJK characters (`é`, `日`), NULL argument validation, and nonexistent path handling.
2. **Windows Cross-Compilation**: `zig cc -target x86_64-windows-gnu` compiles the path helper test and DNN loader
   test with `-Wall -Wextra -Werror` (the DNN harness suppresses its pre-existing Windows-only unused static helpers).
3. **Windows Win32 Execution**: The cross-compiled binary runs under Wine, exercising `MultiByteToWideChar`, `_wfopen`,
   `_wopen`, `_wfullpath`, `_wstat64`, `_wmkdir`, `_wremove`, `GetFileAttributesW`, and `EILSEQ` error translation.
4. **Production Seams**: Native and Zig-cross-compiled Win64 Meson tests execute under Wine for public output
   writing, DNN model/sidecar loading, and CAMBI heatmap creation as well as the compatibility-helper unit test.

## 6. CLI Unicode Argument Follow-up (2026-09-25)

The library contract alone did not protect a path supplied to the Windows CLI. The binaries still entered through
`main(int, char **)`, so the C runtime could encode the Unicode command line through an active ANSI code page before
`vmaf.cpp` received it. A `CreateProcessW` reproducer passed `référence_日本.yuv` and `déformé_日本.yuv` to the exact
base binary; its narrow `main` received a path ending in `référence_??.yuv`, failed to open the input, and exited 255.

The follow-up uses the platform's `wmain(int, wchar_t **)` entry point, which Microsoft documents as supplying wide
arguments, then converts every token with a two-pass `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)` call.
The size probe determines the exact allocation and the strict flag rejects invalid UTF-16 instead of substituting a
replacement character. See Microsoft's [`wmain` documentation](https://learn.microsoft.com/en-us/cpp/c-language/using-wmain?view=msvc-170)
and [`WideCharToMultiByte` reference](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte).
GNU-style Windows links select `wmainCRTStartup` with `-municode`; MSVC-style links infer the wide entry point. The
existing parser and execution path remain shared with the unchanged POSIX `main`.

| Option | Benefit | Rejected cost |
| --- | --- | --- |
| `wmain` plus strict UTF-8 conversion (chosen) | Uses the CRT's authoritative Unicode argv parsing; one bounded conversion before existing parsing | Windows-only entry shim and GNU-linker startup flag |
| Keep narrow `main` | No source change | Leaves correctness dependent on the process ANSI code page |
| Reparse `GetCommandLineW` with `CommandLineToArgvW` | Also starts from Unicode | Duplicates argv parsing and adds a Shell32 allocation/link dependency |
| Rely on an active-code-page manifest | Keeps `main` | Process-global behavior and runner/toolchain support remain external to the executable |

The same Windows-only Meson regression now creates exact wide input/output paths, launches `vmaf.exe` with
`CreateProcessW`, and verifies that the requested Unicode output exists and is non-empty. Under the Zig Win64/Wine
cross build it fails on the base commit and passes after the entry-point conversion. This is an implementation bug
fix completing ADR-1182's already-accepted UTF-8 contract, not a new architecture decision; no new ADR is needed.
