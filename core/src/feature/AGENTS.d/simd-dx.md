---
paths:
  - core/src/feature/simd_dx.h
invariant: SIMD DX framework macros and recurring vector kernel patterns across targets.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# SIMD DX Framework Macros and Recurring Patterns

- **`simd_dx.h` DX macros** (fork-local, ADR-0140): header
  [`simd_dx.h`](../simd_dx.h) is fork-internal and has no upstream
  equivalent. On rebase, keep fork's version. macros
  (`SIMD_WIDEN_ADD_F32_F64_*`, `SIMD_ALIGNED_F32_BUF_*`,
  `SIMD_LANES_*`) encode ADR-0138 / ADR-0139 bit-exactness patterns
  by construction — changing their expansion without auditing
  three SSIM / convolve consumers (`ssim_accumulate_*`,
  `iqa_convolve_*`) is bit-exactness break waiting to happen.
  Macro names are ISA-suffixed on purpose; do not collapse them
  into cross-ISA aliases (fork's SIMD policy rules out
  Highway / simde / xsimd — see user memory
  `feedback_simd_dx_scope.md`).
