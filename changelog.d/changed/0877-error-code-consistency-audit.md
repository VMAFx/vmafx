- refactor(feature): convert `return -1` (malloc failure) to `return -ENOMEM`
  in the fork-added MS-SSIM decimate dispatcher and its three SIMD
  specialisations (scalar, AVX2, AVX-512, NEON). Aligns the four TUs with
  libvmaf's internal negative-errno convention and tightens the public
  docstring in `core/src/feature/ms_ssim_decimate.h` from "0 on success,
  non-zero on allocation failure" to "0 on success, -ENOMEM on allocation
  failure". Caller `ms_ssim.c` uses a truthy check and is unaffected.
  See [ADR-0877](../docs/adr/0877-error-code-consistency-audit.md).
