<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1182: Windows UTF-8 Path Contract (vmaf_fopen_utf8 / vmaf_open_utf8)

- **Status**: Accepted
- **Date**: 2026-09-05
- **Deciders**: Lusoris Maintainers
- **Tags**: `core`, `compat`, `windows`, `utf8`

## Context

Upstream Netflix/vmaf issue #1568 reported that file path arguments containing non-ASCII UTF-8 characters
(such as accented characters, CJK characters, and spaces or symbols) fail on Windows when passed to
`libvmaf` or its CLI tools. On Windows, the standard C runtime `fopen` and `_open` interpret `const char *`
path strings using the legacy ANSI code page (e.g. Windows-1252), which fails to open or mangles filenames
encoded in UTF-8.

On POSIX platforms (Linux, macOS), filesystem paths are byte sequences and UTF-8 works transparently
with standard `fopen(3)` and `open(2)`.

To provide a consistent, uniform cross-platform contract where all path arguments in libvmaf are UTF-8 on
every platform, libvmaf requires a dedicated UTF-8 path compatibility layer.

## Decision

We introduce `core/src/compat/path_utf8.h` and `core/src/compat/path_utf8.c`, exporting two public
compatibility functions:

- `VMAF_EXPORT FILE *vmaf_fopen_utf8(const char *path, const char *mode);`
- `VMAF_EXPORT int vmaf_open_utf8(const char *path, int flags, int mode);`

Implementation details:

1. **Windows (`_WIN32`)**: Paths and modes are converted to wide strings (`wchar_t`) using
   `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`. Invalid UTF-8 sequences immediately map to
   `errno = EILSEQ` and return `NULL` (or `-1`). Buffers are strictly bounded by `UTF8_PATH_MAX` (4096)
   and `UTF8_MODE_MAX` (32) conforming to NASA/JPL Power of 10 Rule 2. Files are opened using `_wfopen`
   and `_wopen`.
2. **POSIX**: `vmaf_fopen_utf8` and `vmaf_open_utf8` pass through directly to standard `fopen(3)` and
   `open(2)` without intermediate copies.
3. **Internal and Tool Adoption**: All narrow `fopen`, `_open`, and `open` calls across `core/src/libvmaf.c:3312`
   and the fork-added call sites (`core/tools/vmaf.cpp`, `core/src/read_json_model.{c,cpp}`,
   `core/tools/vmaf_per_shot.c`, `core/tools/vmaf_roi.c`, `core/tools/vmaf_bench.c`, `core/tools/vmaf_vpl.c`,
   `core/src/dnn/model_loader.c`, and `core/src/interop/pelorus_qp_report_csv.c`) are replaced with their
   `vmaf_*_utf8` counterparts.
4. **FFmpeg Filter**: Patch `0005` passes `log_path` raw to `libvmaf`; with libvmaf establishing a universal
   UTF-8 path contract, no patch change is needed for log file writing.
5. **Scope Boundary**: CLI `wmain`/argv conversion in `core/tools/vmaf.cpp` is out of scope for this library-level
   contract and tracked as a follow-up state item.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Option A (Chosen)**: Centralized compatibility layer (`compat/path_utf8.{h,c}`) exporting `vmaf_fopen_utf8` and `vmaf_open_utf8` | Single source of truth; strictly bounded buffers (Po10); transparent pass-through on POSIX; zero ABI disruption | Requires routing file open calls through helper functions | Cleanest design, satisfies Netflix#1568, and adheres to JPL coding standards |
| **Option B**: Inline `#ifdef _WIN32` with `MultiByteToWideChar` at every call site | Avoids helper function declarations | Massive code duplication across 17+ call sites; high risk of buffer overflow or inconsistent errno mapping | Violates DRY and JPL Power of 10 maintainability |
| **Option C**: Change public libvmaf API to accept `wchar_t *` on Windows | Native Windows API type | Breaks public C API compatibility; breaks Go bindings, FFmpeg filter, and portable cross-platform callers | Unacceptable breaking change to C API |

## Consequences

- **Positive**: Complete support for non-ASCII UTF-8 file paths on Windows across model loading, video reading, sidecar emission, benchmark datasets, and JSON/XML output logs.
- **Negative**: Negligible CPU conversion overhead on Windows when opening files.
- **Neutral / follow-ups**: Documented in `docs/api/index.md`; verified by `core/test/test_path_utf8.c`; follow-up row for Windows CLI `wmain` Unicode argv handling.

## References

- `req`: "Part B (Netflix/vmaf#1568): Windows UTF-8 path contract with core/src/compat/path_utf8.{h,c} exporting vmaf_fopen_utf8 and vmaf_open_utf8... The wmain/argv conversion in vmaf.cpp is OUT OF SCOPE (note it as a follow-up row). ffmpeg patch 0005:130-131/:314 passes log_path raw — no change needed once the API is UTF-8 (say so)."
- Netflix/vmaf#1568: UTF-8 path support on Windows
- NASA/JPL Power of 10 Rules 2 (bounded buffers) and 5 (assertion density)
