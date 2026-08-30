# AGENTS.md — core/src/feature/iqa

Orientation for agents working on the IQA (Image Quality Assessment)
scalar-reference tree — `iqa_*` helpers underlying SSIM, MS-SSIM, and
the shared 11-tap Gaussian convolve. Parent: [../AGENTS.md](../AGENTS.md).

## Scope

```text
feature/iqa/
  convolve.{c,h}              # iqa_convolve / iqa_convolve_2d / 1D-separable scalar
  decimate.{c,h}              # iqa_decimate (used by MS-SSIM scalar)
  iqa.h                       # Public-facing iqa_* declarations
  iqa_options.h               # IQA_CONVOLVE_2D / IQA_CONVOLVE_1D / IQA_BND_* enums
  iqa_os.h                    # OS-portability shims (alignment, restrict)
  math_utils.{c,h}            # log10 / approximate float helpers
  ssim_accumulate_lane.h      # ADR-0139 single-lane scalar-double reduction (shared with SIMD)
  ssim_simd.h                 # SIMD dispatch function pointer typedefs
  ssim_tools.{c,h}            # iqa_ssim / ssim_compute_stats / ssim_init_args + workspace
```

Provenance: this tree was originally tdistler's standalone IQA library
(2011, BSD-2-Clause; see file headers). Netflix imported it; the fork
has accumulated several load-bearing modifications on top.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md)).
- **The scalar code in this directory IS the bit-exact reference**
  for AVX2 / AVX-512 / NEON paths in `../x86/` and `../arm64/`.
  Touching `iqa_convolve` or `ssim_accumulate_default_scalar`
  without paired SIMD updates is a bit-exactness break.
- **`#pragma STDC FP_CONTRACT OFF`** is implicit at TU level via the
  parent build flags; do not reintroduce `fmaf` calls.
- **No reserved identifiers** (ADR-0148): the entire tree was
  swept for leading-underscore symbols (`_iqa_*` / `struct _kernel`
  / `_ssim_int` / underscore-prefixed header guards). Do not
  reintroduce any of those spellings on rebase.

## Rebase-sensitive invariants

- **`ssim_tools.c::ssim_accumulate_default_scalar` defines the
  ADR-0139 reduction shape.** The two `2.0 *` literals
  (`2.0 * ref_mu[i] * cmp_mu[i] + C1` and `2.0 * srsc + C2`) are
  C `double` literals — they promote the float operands to double
  before the multiply. The final `l*c*s` product is also in double.
  All three SIMD accumulators (AVX2, AVX-512, NEON) match this by
  doing the `2.0 *` numerator + division + final product per-lane
  in scalar double via [`ssim_accumulate_lane.h`](ssim_accumulate_lane.h).
  **If upstream ever changes either `2.0` to `2.0f`, or
  restructures the l/c numerators, ALL three SIMD variants
  need a matching rewrite in the same PR.**

- **`convolve.c::iqa_convolve` taps are widen-then-add** (ADR-0138):
  `sum += img[i] * k[j]` where the multiply is `float * float` and
  the running sum is `double`. The AVX2 / AVX-512 / NEON twins in
  `../x86/convolve_*.c` and `../arm64/convolve_neon.c` mirror
  this with single-rounded `_mm256_cvtps_pd(_mm256_mul_ps(...))`
  / `vcvt_f64_f32(vmul_f32(...))` chains. **No FMA, no pre-widen
  of the kernel taps.** Changing the scalar pattern requires
  matching all three SIMD variants.

- **TU-static rename `_calc_scale` → `iqa_calc_scale`** (fork-local,
  ADR-0148). Keep the non-reserved spelling on rebase.

- **`iqa_convolve` helper decomposition** (fork-local, ADR-0146):
  the function is split into `iqa_convolve_horizontal_pass` +
  `iqa_convolve_vertical_pass` composed by
  `iqa_convolve_1d_separable` (for `IQA_CONVOLVE_1D`) and
  `iqa_convolve_2d`. The pass ordering preserves ADR-0138 exactly.
  **On rebase**: prefer upstream's shape only if it maintains the
  widen-then-add invariant; otherwise keep the fork's split and
  re-document divergence in
  [`docs/rebase-notes.md`](../../../../docs/rebase-notes.md).

- **`iqa_ssim` helper decomposition** (fork-local, ADR-0146):
  split into `ssim_workspace_alloc` / `_free` plus
  `ssim_compute_stats` and `ssim_init_args` around an explicit
  `struct ssim_workspace`.
  Same on-rebase rule as the convolve split.

- **`ssim_accumulate_lane.h` is the single source of truth** for
  the per-lane reduction. AVX2 / AVX-512 / NEON each pre-compute
  the float-valued intermediates (`srsc`, `l_den`, `c_den`,
  `sv_f`) in vector float, spill to an aligned float buffer,
  then call `ssim_accumulate_lane` per lane. Changing the helper
  signature is a four-TU change (this header + 3 SIMD callers).

- **`integer_ssim.c` registry wiring is fork-side** (see parent
  AGENTS.md "vmaf_fex_ssim is registered fork-side, not upstream"):
  three upstream-mirror surfaces (`feature_extractor.c` registry
  row, the matching `extern`, and the `#include "config.h"` in
  `integer_ssim.c`) plus one fork-local meson-build line wire
  the integer-SSIM scalar path so `vmaf --feature ssim` resolves
  at the CLI. Re-check on every upstream sync.

- **NOLINT brackets in `integer_ssim.c`** (ADR-0148 carve-outs):
  scoped `NOLINTBEGIN/END(clang-analyzer-security.ArrayBound)`
  around the inner kernel loops in `ssim_accumulate_row` and
  `ssim_reduce_row_range` — the `k_min` / `k_max` clamping is
  provably correct but the analyzer can't follow it across the
  helper boundary. Keep these brackets verbatim on rebase.

## Twin-update awareness

When editing any file in this directory, walk these lists before
landing:

- **`ssim_tools.c` / `ssim_accumulate_lane.h`** → check
  `../x86/ssim_avx2.c`, `../x86/ssim_avx512.c`,
  `../arm64/ssim_neon.c`.
- **`convolve.c`** → check `../x86/convolve_avx2.c`,
  `../x86/convolve_avx512.c`, `../arm64/convolve_neon.c`,
  `../common/convolution_avx.c` (separate ADR-0143 helper tree).
- **`decimate.c`** → check `../ms_ssim_decimate.c` and the four
  byte-identical SIMD twins (`../x86/ms_ssim_decimate_*.c`,
  `../arm64/ms_ssim_decimate_neon.c`).

## Upstream-sync notes

This tree carries the original tdistler 2011 BSD-2-Clause headers
on `convolve.c`, `decimate.c`, `ssim_tools.c` (see file preambles).
Netflix and the fork preserve those headers verbatim. The `ssim_tools.c`
header has a 2016 `zli-nflx@netflix.com` modification note documenting
the mean luminance / contrast / structure outputs — keep it.

## Governing ADRs

- [ADR-0138](../../../../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md) —
  `iqa_convolve` widen-then-add bit-exactness.
- [ADR-0139](../../../../docs/adr/0139-ssim-simd-bitexact-double.md) —
  SSIM accumulate per-lane scalar-double reduction.
- [ADR-0146](../../../../docs/adr/0146-nolint-sweep-function-size.md) —
  IQA / VIF SIMD helper decomposition.
- [ADR-0148](../../../../docs/adr/0148-iqa-rename-and-cleanup.md) —
  reserved-identifier rename + NOLINT carve-outs.
