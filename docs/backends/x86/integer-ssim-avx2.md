<!-- markdownlint-disable MD013 -->
# Integer SSIM AVX2 SIMD path

**Added**: 2026-05-29 ([ADR-0784](../../adr/0784-integer-ssim-avx2.md))
**Scope**: `core/src/feature/x86/integer_ssim_avx2.c`, `core/src/feature/x86/integer_ssim_avx2.h`

No flag or option is involved: on an x86 host with AVX2 the `ssim` extractor
uses this path automatically and returns the same scores as the scalar code.
The extractor itself is described in [SSIM](../../metrics/ssim.md).

## Overview

The `ssim` feature extractor uses a two-pass separable Gaussian filter to compute
per-frame SSIM scores from integer (fixed-point) pixel data.  The horizontal pass
— which accumulates five weighted moment sums (`mux`, `muy`, `x2`, `xy`, `y2`) per
output pixel — was previously entirely scalar.  The AVX2 path vectorises that pass.

## What is accelerated

`ssim_accumulate_row()` in `core/src/feature/integer_ssim.c` is replaced at runtime by one of:

| Function | Pixel depth | SIMD width |
| --- | --- | --- |
| `integer_ssim_accumulate_row_avx2` | 8 bpc (uint8) | 8 output pixels per iteration |
| `integer_ssim_accumulate_row_16_avx2` | 9-16 bpc (uint16) | 4 output pixels per iteration |

The vertical reduction pass and the SSIM formula computation remain scalar.

## Bit-exactness guarantee

All intermediate arithmetic is integer (no float).  The 8bpc path accumulates
products in `int32` lanes (the largest tap term is 256 × 255² < 2²⁵, and the
sum over the kernel taps still fits `int32`), then widens to `int64` at store
time.  The 16bpc path accumulates directly in `int64` lanes with
`_mm256_mul_epi32`, forming `(w * s) * s` rather than `w * (s * s)` because
`s * s` can reach 65535² and set bit 31 of the 32-bit operand.  The output
`integer_ssim_moments_t` fields are therefore bit-identical to the scalar
reference.

## Runtime dispatch

The extractor's `init()` function in `integer_ssim.c` queries
`vmaf_get_cpu_flags()`.  When `VMAF_X86_CPU_FLAG_AVX2` is set (and `ARCH_X86`
is defined), the `IntegerSsimState.accum8` and `.accum16` function pointers
are set to the AVX2 variants.  On non-x86 hosts or hosts without AVX2, the
scalar wrappers are used unchanged.

## Boundary handling

Pixels within `hkernel_offs` of either edge have a truncated kernel window.
Both variants process the left and right boundary pixels (and any interior
remainder shorter than one SIMD block) with a per-pixel scalar routine inside
the same function, and run the SIMD loop only over the interior where every
tap is in bounds.

## Test coverage

`core/test/test_integer_ssim_simd.c` (meson test `test_integer_ssim_simd`,
suites `fast` and `simd`, built only for x86 and x86_64) compares the AVX2
rows with a scalar reference using `memcmp` over every `integer_ssim_moments_t`:

- 8bpc random rows, 64 pixels wide, 4 seeds.
- 16bpc random rows in the 10-bit range, 64 pixels wide, 4 seeds.
- 16bpc full-range alternating rows (`test_integer_ssim_avx2_16bpc_bright`),
  a regression test for the $s \cdot s \ge 2^{31}$ overflow described above.
- 8bpc uniform rows, all-white source against all-black distorted.
- Narrow row, width 1: all boundary, no SIMD iteration.

Run it with `python3 scripts/ci/run_meson_test.py -- -C build test_integer_ssim_simd`
or through `--suite=fast`.
