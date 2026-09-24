- **Windows UTF-8 path contract** (`core/src/compat/path_utf8.{h,c}`):
  resolves failures opening file paths containing non-ASCII UTF-8 characters on Windows
  (Netflix#1568, ADR-1182). Implements internal UTF-8 opening, canonicalization,
  metadata, directory-creation, and removal helpers using `MultiByteToWideChar` and
  wide-character runtime APIs on Windows
  with bounded buffers (NASA/JPL Power of 10) and transparent pass-through on POSIX.
- **Library and tools migration**:
  replaces narrow `_open`/`open`/`fopen` calls in `core/src/libvmaf.c` and fork-added
  sites across CLI, model loader, CAMBI heatmap, and sidecar paths. The vendored
  Pelorus CSV parser remains explicitly open until fixed in Pelorus and re-vendored.
- **Unit test suite** (`core/test/test_path_utf8.c`):
  validates round-trip open/canonicalize/stat/mkdir/remove behavior for non-ASCII
  UTF-8 paths; production-seam tests cover the public output writer, DNN loader,
  and CAMBI heatmaps.
