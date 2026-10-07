---
paths:
  - core/src/feature/brisque.c
  - core/src/feature/ciede.c
  - core/src/feature/delta_e_itp.c
  - core/src/feature/float_adm.c
  - core/src/feature/float_moment.c
  - core/src/feature/float_motion.c
  - core/src/feature/float_ms_ssim.c
  - core/src/feature/float_psnr.c
  - core/src/feature/float_ssim.c
  - core/src/feature/float_vif.c
  - core/src/feature/integer_adm.c
  - core/src/feature/integer_ssim.c
  - core/src/feature/integer_vif.c
  - core/src/feature/niqe.c
  - core/src/feature/pu21.c
  - core/src/feature/speed.c
  - core/src/feature/ssimulacra2.c
  - core/test/test_libc_named_internal_functions.py
invariant: no static C function named after C library function; extractor close callback is close_fex.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Extractor callbacks never take a C library name

## `close_fex`, not `close` (T-DARWIN-LTO-STATIC-CLOSE-COLLISION-2026-10-06)

Upstream Netflix/vmaf names every CPU extractor's close callback `static int
close(VmafFeatureExtractor *)`. macOS headers declare `close()` with
assembler label (`__DARWIN_ALIAS_C(close)` = `__asm("_close")`). Full LTO
(`b_lto=true`, release default) merges all modules: label keeps
declaration's IR name apart from static one, so IR linker does not
rename static, and both emit as `_close`. Every libc `close()` call in
merged module then branches into extractor's close with file descriptor
for `fex` (macOS `test_adm_coverage` SIGSEGV; fdopen() failure paths of
`libvmaf.c`, `cambi.c`, `svm.cpp`). Linux glibc has no label: no collision.

fork names callback `close_fex` (GPU twins already use
`close_fex_cuda` / `_hip` / `_metal`). upstream sync that touches one of
these files keeps fork's name; new extractor or helper never defines
`static` function named after C library or POSIX function.
`core/test/test_libc_named_internal_functions.py` (fast suite) fails on one.
