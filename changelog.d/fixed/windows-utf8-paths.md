- **Windows UTF-8 path contract** (`core/src/compat/path_utf8.{h,c}`):
  resolves failures opening file paths containing non-ASCII UTF-8 characters on Windows
  (Netflix#1568, ADR-1182). Implements `vmaf_fopen_utf8` and `vmaf_open_utf8` using
  `MultiByteToWideChar` and wide-character runtime APIs (`_wfopen`, `_wopen`) on Windows
  with bounded buffers (NASA/JPL Power of 10) and transparent pass-through on POSIX.
- **Library and tools migration**:
  replaces narrow `_open`/`open`/`fopen` calls in `core/src/libvmaf.c:3312` and fork-added
  sites across CLI, model loader, and sidecar generators.
- **Unit test suite** (`core/test/test_path_utf8.c`):
  validates round-trip write and read-back for non-ASCII UTF-8 filenames, wide Win32
  filesystem attributes, and error handling.
