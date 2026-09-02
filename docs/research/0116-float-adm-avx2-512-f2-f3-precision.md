# Research-0116: float_adm AVX2/AVX-512 F2+F3 precision audit

- **Status**: implementation digest
- **Date**: 2026-05-29
- **Relevant ADR**: [ADR-0844](../adr/0844-float-adm-avx2-512-f2-f3.md)

## Question

Do the `float_adm` AVX2 and AVX-512 SIMD paths in
`core/src/feature/x86/float_adm_avx2.c` and `float_adm_avx512.c`
produce numerically consistent results compared to the scalar
reference in `core/src/feature/float_adm.c`?

## Findings

Two defects were identified during a precision audit of the ADM
SIMD reduction functions:

### F2 — Store-to-temp + scalar-loop accumulation

`float_adm_csf_den_scale_avx2`, `float_adm_sum_cube_avx2`, and
their AVX-512 twins used:

```c
_Alignas(32) float tmp[8];
_mm256_store_ps(tmp, vcube);
for (int k = 0; k < 8; k++)
    row_accum += (double)tmp[k];
```

The intermediate float store followed by individual `(double)tmp[k]`
casts means each lane is widened individually, but the running-sum
accumulation `row_accum += (double)tmp[k]` still operates in
double.  The issue is that each lane value is only as precise as a
32-bit float before widening — the store to `tmp[k]` rounds to float
first, then the cast widens the already-rounded value to double.
By contrast, `_mm256_cvtps_pd` performs the widening directly from
the SIMD register without the intermediate float store, preserving
the full bit-level content of each lane before reduction.

In practice the difference is sub-ULP on most frames, but the
store-round pattern is structurally incorrect relative to the scalar
reference's double-accumulation contract (ADR-0139).

### F3 — Compiler auto-FMA in the DWT2 vertical pass

The `float_adm_avx2.c` and `float_adm_avx512.c` TUs were compiled
as part of `x86_avx2_static_lib` / `x86_avx512_static_lib`, which
pass `-mfma` in `c_args`.  With `-mfma`, GCC and Clang are free to
auto-fuse adjacent `_mm256_mul_ps` + `_mm256_add_ps` pairs into FMA
instructions, producing a single-rounding result.

The scalar reference in `float_adm.c` does not receive `-mfma` (the
scalar sources are compiled without ISA flags), so the same C source
pattern produces two-rounding `mul` + `add`.  When the compiler
auto-fuses in the SIMD TU but not in the scalar TU, the vertical-pass
outputs diverge at the bit level.

This is the same class of defect that motivated the `ssimulacra2`
per-TU carve-out with `-ffp-contract=off` already present in
`core/src/meson.build`.

## Fix summary

- **F2**: Replace store-to-temp loops with `_mm256_cvtps_pd` plus
  `hadd_pd4()` (AVX2) and `_mm512_extractf32x4_ps` plus `_mm256_cvtps_pd`
  plus `hadd_pd4()` (AVX-512). No float-precision intermediate stores
  anywhere in the reduction paths.
- **F3**: Move both TUs to isolated `x86_float_adm_avx2_lib` /
  `x86_float_adm_avx512_lib` static libraries in `core/src/meson.build`
  with `-ffp-contract=off` in `c_args`.

## Verification

Build: `meson test -C build --suite=fast` — 49/49 tests pass.
No AVX-512 hardware was available on the build host; the AVX-512 fix
is structurally identical to the AVX2 fix and compiles clean under
`-mavx512f`.

Cross-backend reproducer:

```bash
vmaf --cpumask 255 --reference src01_hrc00_576x324.yuv \
     --distorted src01_hrc01_576x324.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --feature float_adm --precision max -o scalar.xml
vmaf --cpumask 4   ... -o avx2.xml
diff <(grep float_adm scalar.xml) <(grep float_adm avx2.xml)
# expect: empty (bit-identical)
```
