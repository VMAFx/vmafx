- **x86 ADM SIMD macro hygiene and clang-tidy zero-warning gate**:
  Cleaned macro definitions and pointer arithmetic in `core/src/feature/x86/adm_avx2.c`
  (reduced from 570 to 0 warnings) and `core/src/feature/x86/adm_avx512.c` (reduced
  from 561 to 0 warnings). Parenthesized all macro parameters and expansions in
  thresholding and accumulation macros (`ADM_CM_THRESH_*`, `I4_ADM_CM_THRESH_*`,
  `ADM_CM_ACCUM_ROUND*`), cast pointer arithmetic products to `(ptrdiff_t)` to prevent
  implicit widening, removed dead `print_*` debug macros in `adm_avx512.c`, included
  `adm_avx2.h` in `adm_avx2.c` for internal linkage consistency, and added inline
  ADR-0138/0139 and ADR-0141 citations to preserve bit-exact register allocation and
  reduction order across all 11 AVX2 and AVX-512 kernels.
