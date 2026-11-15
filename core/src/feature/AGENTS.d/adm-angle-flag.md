---
paths:
  - core/src/feature/adm_tools.c
  - core/src/feature/adm_tools.h
invariant: adm_angle_flag has exactly one definition across floating-point and integer paths.
area: adm
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# adm_angle_flag Unified Definition Invariant

## `angle_flag` has exactly one definition (ADR-1194)

integer-ADM 1-degree angle test lives in
[`adm_angle_flag.h`](../adm_angle_flag.h) and nowhere else. Do not re-inline it
into backend, and do not "improve" it.

- `adm_angle_flag_fp64()` is **golden-frozen** upstream expression: narrow
  each int64 operand to `float`, then compare in `double`. narrowing is
  lossy past 24-bit significand — that is not bug to fix, it is
  value Netflix golden assertions encode (CLAUDE.md rule 1). scalar
  CPU path, CUDA and HIP call it, at both scale 0 and scales 1-3.
- `adm_angle_flag_i64()` returns **bit-identical** result using only
  64-bit integers. SYCL calls it because
  [`sycl/integer_adm_sycl.cpp`](../sycl/integer_adm_sycl.cpp) must contain no
  binary64 instruction at all (one fp64 op anywhere in that translation unit
  makes runtime reject whole SPIR-V module on Arc A-series and
  iGPUs). [`metal/integer_adm.metal`](../metal/integer_adm.metal) mirrors it
  by hand because MSL has no `double` type.

Two consequences for anyone editing this area:

1. **MSL copy is manual mirror.** `iadm_angle_flag()` in
   `metal/integer_adm.metal` is line-for-line translation of
   `adm_angle_flag_i64()`. They must be edited together, in same commit.
   Nothing in build catches drift between them — Metal is not built on
   Linux.
2. **`ADM_ANGLE_FLAG_MC` / `ADM_ANGLE_FLAG_D` encode constant.**
   integer form hard-codes significand of `(float)cos(1deg)^2`
   (`0x3F7FEC0A`, `MC = 16772106`, `D = 2^24 - MC = 5110`), and MSL mirror
   repeats `D`. If constant ever changes, all three move together.
   `core/test/test_adm_angle_flag.c` asserts relationship, so partial
   edit fails `fast` suite rather than silently shifting scores.

flipped `angle_flag` selects other branch of `decouple()`'s enhancement
gain limit, so it moves `adm` scores directly. four historical spellings
of this predicate disagreed on about 4e-5 of near-parallel scale-0 band
quadruples; see
[`docs/research/2030-adm-angle-flag-fp64-free.md`](../../../../docs/research/2030-adm-angle-flag-fp64-free.md).
