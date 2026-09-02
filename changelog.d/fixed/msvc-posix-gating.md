`Build — Windows MSVC + CUDA` and `Build — Windows MSVC + oneAPI SYCL` matrix
legs are now green. Two independent portability gaps were present after PR #1274
(ADR-0515) fixed the MinGW64 leg:

- `core/src/feature/x86/vif_avx512.c`: the ADR-0503 noinline helpers
  `vif_subsample_rd_8_vert_j` and `vif_subsample_rd_8_horiz_j` used bare
  `__attribute__((noinline, noclone))`, which is valid GCC/Clang syntax but
  causes a fatal `C2143` / `C2059` syntax error on MSVC `cl.exe`. Fixed by
  introducing the `VMAF_NOINLINE_NOCLONE` portability macro that maps to
  `__declspec(noinline)` under `_MSC_VER` and `__attribute__((noinline,
  noclone))` under `__GNUC__` / `__clang__`.

- `core/tools/yuv_input.c`: `yuv_check_file_size` called `fstat()` and
  `S_ISREG()` — POSIX names absent from MSVC's `<sys/stat.h>`. Fixed by
  adding `_WIN32` shims (`fstat` → `_fstat64`, `struct stat` → `struct __stat64`,
  `S_ISREG` macro, `typedef __int64 off_t`) in the existing `#ifdef _WIN32`
  block so the function body stays portable.

ADR-0519.
