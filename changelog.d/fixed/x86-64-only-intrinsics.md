- **The AVX2 and AVX-512 sources no longer use x86-64-only intrinsics.**
  `_mm_extract_epi64` in the ADM kernels and `_mm_cvtsi128_si64` in the PSNR
  kernel were the cause of Netflix#1481 (no 32-bit x86 build with asm). They
  now go through 32-bit-safe forms. Nothing changes on x86-64, and 32-bit x86
  remains unsupported.
