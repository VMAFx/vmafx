- **10 unchecked `malloc`/`calloc` sites in 3 test files now NULL-check
  their allocations (Round 27 audit D.1).** `test_ssimulacra2_simd.c`
  (`test_multiply`, `test_xyb`, `test_downsample`, `test_ssim`, `test_edge`,
  `test_blur`, `test_ptlr_one`), `test_framesync.c` (per-frame loop), and
  `test_pic_preallocation.c` (per-thread setup) all dereferenced `malloc`
  return values without checking for NULL, causing SIGSEGV under ASan
  `MALLOC_PERTURB_=198`. Fixes use the established project idioms: consolidated
  `if (!ptr) { free(...); return "malloc failed"; }` for multi-alloc SIMD tests
  and `mu_assert` for loop-local allocations. `core/test/AGENTS.md` gains the
  invariant rule for future test authors. ADR-0971.
