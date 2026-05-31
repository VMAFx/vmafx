- printf-format portability sweep (ADR-0876, CERT FIO47-C / MISRA
  21.6): switched fixed-width integer log / debug sites in
  fork-added `core/src/libvmaf.c` (tiny-model loader),
  `core/src/sycl/common.cpp` (frame-counter timing prints),
  `core/src/sycl/dmabuf_import.cpp` (DRM modifier hex prints), and
  `core/test/test_motion_v2_simd.c` (SAD divergence log) from the
  `(unsigned long)` + `%lu` / `(long long)` + `%lld` cast idiom to
  `<inttypes.h>` PRI macros (`PRIu64` / `PRId64` / `PRIx64`). The
  `(unsigned long)` + `%lu` form silently truncated `uint64_t` on
  Windows LLP64 (where `unsigned long` is 32 bits); the PRI macros
  expand correctly across LP64, LLP64, and 32-bit ILP32. Three
  call sites were verified not-bugs and left alone: `off_t` +
  `(long long)` in `core/tools/yuv_input.c` (correct CERT idiom
  for non-fixed-width POSIX types), Windows `DWORD` +
  `(unsigned long)` in `core/test/test_public_api_score.c`
  (`DWORD` is exactly `unsigned long` on Windows), and the
  upstream `print_128_64` debug macro in
  `core/src/feature/x86/adm_avx512.c` (reserved for upstream
  sync). CPU-only build + all 49 fast tests pass.
