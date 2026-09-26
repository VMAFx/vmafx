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

We introduce `core/src/compat/path_utf8.h` and `core/src/compat/path_utf8.c`, providing internal
compatibility functions:

- `FILE *vmaf_fopen_utf8(const char *path, const char *mode);`
- `int vmaf_open_utf8(const char *path, int flags, int mode);`
- `char *vmaf_fullpath_utf8(const char *path, char *resolved, size_t resolved_size);`
- `int vmaf_path_info_utf8(const char *path, VmafPathInfo *info);`
- `int vmaf_mkdir_utf8(const char *path, mode_t mode);`
- `int vmaf_remove_utf8(const char *path);`

Implementation details:

1. **Windows (`_WIN32`)**: Paths and modes are converted to wide strings (`wchar_t`) using
   `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`. Invalid UTF-8 sequences immediately map to
   `errno = EILSEQ` and return `NULL` (or `-1`). Buffers are strictly bounded by `UTF8_PATH_MAX` (4096)
   and `UTF8_MODE_MAX` (32) conforming to NASA/JPL Power of 10 Rule 2. Files are opened using `_wfopen`
   and `_wopen`; canonicalization, metadata, and directory creation use `_wfullpath`, `_wstat64`, and
   `_wmkdir` respectively. Test cleanup uses `_wremove` so a passing test cannot leave the Unicode file behind.
2. **POSIX**: The compatibility functions delegate to standard `fopen(3)`, `open(2)`, `realpath(3)`,
   `stat(2)`, `mkdir(2)`, and `remove(3)` without changing path bytes.
3. **Internal and Tool Adoption**: VMAFx-owned path operations in `core/src/libvmaf.c`,
   `core/src/read_json_model.{c,cpp}`, `core/src/dnn/model_loader.c`, `core/src/feature/cambi.c`,
   `core/src/feature/mkdirp.cpp`, and tools (`vmaf.cpp`, `vmaf_bench.c`, `vmaf_per_shot.c`,
   `vmaf_roi.c`, `vmaf_vpl.c`) route through the UTF-8 compatibility layer. This includes the
   canonicalization and metadata gates that precede a DNN model open and CAMBI's documented
   `heatmaps_path` directory/file creation.
   `core/src/interop/pelorus_qp_report_csv.c` remains untouched per ADR-1113's verbatim Pelorus
   mirror invariant. Its public `pel_x265_csv_parse()` path is therefore an explicit exception until
   the fix originates in `VMAFx/pelorus` and is re-vendored; the original 12-site state item stays open.
4. **FFmpeg Filter**: Patch `0005` passes `log_path` raw to libvmaf's output writer. That writer now owns the
   UTF-8 conversion, so no patch change is needed for log file writing.
5. **Scope Boundary**: CLI `wmain`/argv conversion in `core/tools/vmaf.cpp` is out of scope for this library-level
   contract and tracked as a follow-up state item.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Option A (Chosen)**: Centralized compatibility layer (`compat/path_utf8.{h,c}`) for opening, canonicalizing, inspecting, creating, and removing paths | Single source of truth; strictly bounded buffers (Po10); transparent pass-through on POSIX; zero ABI disruption | Requires routing each filesystem operation through the matching helper | Cleanest design, satisfies Netflix#1568 for VMAFx-owned surfaces, and adheres to JPL coding standards |
| **Option B**: Inline `#ifdef _WIN32` with `MultiByteToWideChar` at every call site | Avoids helper function declarations | Massive code duplication across 17+ call sites; high risk of buffer overflow or inconsistent errno mapping | Violates DRY and JPL Power of 10 maintainability |
| **Option C**: Change public libvmaf API to accept `wchar_t *` on Windows | Native Windows API type | Breaks public C API compatibility; breaks Go bindings, FFmpeg filter, and portable cross-platform callers | Unacceptable breaking change to C API |

## Consequences

- **Positive**: Non-ASCII UTF-8 paths work on Windows across VMAFx-owned model loading, video reading,
  CAMBI heatmaps, sidecar emission, benchmark datasets, and JSON/XML output logs.
- **Negative**: Negligible CPU conversion overhead on Windows when opening files.
- **Neutral / follow-ups**: Documented in `docs/api/index.md`; verified at the helper layer and through
  the output-writer, DNN-loader, and CAMBI production seams. Windows CLI `wmain` Unicode argv handling
  remains a separate state item. The Pelorus CSV exception remains on the original state item until an
  upstream Pelorus change can be re-vendored without violating ADR-1113.

## References

- `req`: "Part B (Netflix/vmaf#1568): Windows UTF-8 path contract with core/src/compat/path_utf8.{h,c} exporting vmaf_fopen_utf8 and vmaf_open_utf8... The wmain/argv conversion in vmaf.cpp is OUT OF SCOPE (note it as a follow-up row). ffmpeg patch 0005:130-131/:314 passes log_path raw — no change needed once the API is UTF-8 (say so)."
- Netflix/vmaf#1568: UTF-8 path support on Windows
- NASA/JPL Power of 10 Rules 2 (bounded buffers) and 5 (assertion density)
