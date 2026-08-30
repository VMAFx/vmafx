- Python harness: the `adm_dwt2_cy` Cython extension builds again.
  `core/src/mem.c` became `core/src/mem.cpp` when the C++23 twins were
  wired into the build (#1133), but `adm_dwt2_cy.pyx` still text-included
  the old `.c` path, so every `tox` leg died with
  `../../../core/src/mem.c: No such file or directory` while compiling
  `adm_dwt2_cy.c`. A C++23 translation unit cannot be `#include`d into
  this C module, so the extern now declares `aligned_malloc` /
  `aligned_free` from `mem.h` (which already guards both with
  `extern "C"`) and `core/src/mem.cpp` is compiled as a real extension
  source. Verified behaviour-preserving: the rebuilt module's output is
  byte-identical to the previously-built `.so` across four geometries
  (48x64, 30x50, 17x33, 64x64) — same SHA-256 over all four bands.
