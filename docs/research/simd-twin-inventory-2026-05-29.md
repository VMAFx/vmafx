# Research digest: SIMD twin coverage inventory (2026-05-29)

## Scope

Full audit of which feature extractors in `core/src/feature/` have
`x86/<feature>_avx2.c`, `x86/<feature>_avx512.c`, and
`arm64/<feature>_neon.c` SIMD twins, and which are missing them.

## Method

1. Listed all `*.c` entry-point files under `core/src/feature/` (excluding
   infrastructure, third-party, and helper files).
2. Cross-checked each against the `x86/` and `arm64/` subdirectories.
3. For every apparent gap, inspected the feature's dispatcher block (the
   `vmaf_get_cpu_flags()` call site) to distinguish "no file" from "file
   exists but is not wired in".
4. Verified historical context: `ansnr` SIMD files were tested and removed
   in PR #38 and follow-up commits; this is a deliberate drop, not a gap.

## Key findings

### Effectively covered via delegation (not real gaps)

- `float_ssim`, `float_ms_ssim`, `ms_ssim`: all delegate through
  `iqa_ssim_set_dispatch()` which selects `ssim_precompute_avx2/neon`,
  `ssim_variance_avx2/neon`, `ssim_accumulate_avx2/neon`, and
  `iqa_convolve_avx2/neon`. Full AVX2 + AVX-512 + NEON coverage through
  shared helpers.
- `float_vif`: delegates through `vif_tools.c`, which contains inline
  `vmaf_get_cpu_flags()` blocks dispatching to `convolution_f32_avx512_s`,
  `convolution_f32_avx512_sq_s`, `convolution_f32_avx512_xy_s`.

### True gap #1: `integer_ssim` — zero SIMD (HIGH leverage)

`integer_ssim.c` (330 lines) has no `#include` for any `x86/` or `arm64/`
header and no `vmaf_get_cpu_flags()` call. Its two hot inner loops
(horizontal row accumulation at line 111, vertical accumulation + SSIM term
at line 153) are fully scalar. This extractor is the default SSIM surface
for all integer-path pipelines, meaning every non-GPU VMAF run on an x86 or
arm64 server runs SSIM with no vectorisation at all.

**Leverage**: AVX2 can process 8 int32 lanes at once for the accumulation
reduction, yielding a projected 4–6x throughput gain on the SSIM hot path.
NEON delivers similar gains on arm64 cloud instances (Graviton, Neoverse).

**Implementation note**: the integer SSIM kernel is structurally similar to
`ssim_avx2.c` (which implements the IQA float path). A new
`x86/integer_ssim_avx2.c` can reuse the same horizontal/vertical separable
accumulation pattern.

### True gap #2: `integer_motion` NEON incomplete (MEDIUM leverage)

`motion_neon.c` exports only `x_convolution_16_neon`. The dispatcher in
`integer_motion.c` has an x86 block (lines 391–401) but no arm64 block at
all, so `y_convolution_8/16_neon` and `sad_neon` are missing. On arm64
servers this means motion estimation runs scalar convolution and SAD.

`integer_motion_v2` does not share this gap: it has a complete `motion_v2_neon.c`
with full pipeline dispatch.

**Leverage**: motion SAD on large frames (1080p, 4K) is the dominant cost
on arm64 hosts without the NEON path. Adding `y_convolution_8/16_neon` and
`sad_neon` to `motion_neon.c` and wiring them into `integer_motion.c` is a
contained change — `motion_v2_neon.c` is the exact template.

### True gap #3: `moment` / `float_moment` AVX-512 (LOWER leverage)

`moment_avx2.c` (2 kernels: `compute_1st_moment_avx2`,
`compute_2nd_moment_avx2`) and `moment_neon.c` exist and are dispatched.
`moment_avx512.c` was never created. The comment in `moment_avx2.h`
references ADR-0179 as the ADR that "closes the only remaining fully-scalar
row" — suggesting AVX-512 was a known follow-up.

**Leverage**: lower than gaps 1 and 2. `float_moment` is a small helper
(two dot-product reductions over a float frame), not a dominant hot path.
AVX-512 with 512-bit EVEX encoding gains 2x over AVX2 only on Skylake-X and
newer; on Ice Lake and later the gain is smaller due to frequency throttling.
Worth doing but not blocking.

## Priority recommendation

| Priority | Gap | ISAs to add | Effort estimate |
| --- | --- | --- | --- |
| 1 | `integer_ssim` AVX2 + NEON | AVX2, NEON (AVX-512 stretch goal) | ~2 days |
| 2 | `integer_motion` NEON completion | NEON only (3 functions) | ~0.5 days |
| 3 | `moment` AVX-512 | AVX-512 only (2 functions) | ~0.5 days |

Gaps 2 and 3 are small enough to batch into a single PR if desired.
Gap 1 should be its own PR given the new `integer_ssim_avx2.c` file and
associated bit-exact parity test.

## Sources

All findings from direct code inspection of the worktree at commit
`61ff5e0565` (master, 2026-05-29). No external sources required.
