# Integer SSIM AVX2 SIMD path

**Added**: 2026-05-29 (ADR-0784)
**Scope**: `core/src/feature/x86/integer_ssim_avx2.c`

## Overview

The `ssim` feature extractor uses a two-pass separable Gaussian filter to compute
per-frame SSIM scores from integer (fixed-point) pixel data.  The horizontal pass
— which accumulates five weighted moment sums (`mux`, `muy`, `x2`, `xy`, `y2`) per
output pixel — was previously entirely scalar.  The AVX2 path vectorises that pass.

## What is accelerated

`ssim_accumulate_row()` in `integer_ssim.c` is replaced at runtime by one of:

| Function | Pixel depth | SIMD width |
| --- | --- | --- |
| `integer_ssim_accumulate_row_avx2` | 8 bpc (uint8) | 8 output pixels per iteration |
| `integer_ssim_accumulate_row_16_avx2` | 9-16 bpc (uint16) | 4 output pixels per iteration |

The vertical reduction pass and the SSIM formula computation remain scalar.

## Bit-exactness guarantee

All intermediate arithmetic is integer (no float).  The 8bpc path accumulates
products in `int32` registers (safe: max value per tap is 256 × 255² < 2²⁷),
then widens to `int64` at store time.  The 16bpc path accumulates directly in
`int64` using `_mm256_mul_epi32` on widened 64-bit operands.  The output
`integer_ssim_moments_t` struct fields are therefore bit-identical to the scalar
reference for any valid input.

## Runtime dispatch

The extractor's `init()` function queries `vmaf_get_cpu_flags()`.  When
`VMAF_X86_CPU_FLAG_AVX2` is set, the `IntegerSsimState.accum8` and `.accum16`
function pointers are set to the AVX2 variants.  On non-x86 hosts or hosts
without AVX2, the scalar wrappers are used unchanged.

## Boundary handling

Pixels within `hkernel_offs` of either edge have a truncated kernel window.
Both variants handle boundary pixels with a per-pixel scalar fallback embedded
in the same function; no separate scalar loop at the call site is needed.

## Test coverage

`core/test/test_integer_ssim_simd.c` runs on every x86 AVX2 build and checks:

- 8bpc random rows (4 seeds, 64 pixels wide) — `memcmp` bit-exact.
- 16bpc random rows (10-bit range, 4 seeds) — `memcmp` bit-exact.
- Adversarial: all-white src / all-black dst uniform rows.
- Narrow row (width = 1): exercises the all-boundary scalar path.

The test is in the `fast` + `simd` suite and runs as part of
`meson test -C build --suite=fast`.

## Performance

Projected 4–6x speedup on the horizontal pass for typical 1080p frames on
AVX2 hosts (e.g., Intel Haswell and later, AMD Zen 1 and later).  Full
frame-level throughput gain depends on the fraction of time spent in the
horizontal pass; the vertical pass and the SSIM formula computation are
not vectorised in this revision.
