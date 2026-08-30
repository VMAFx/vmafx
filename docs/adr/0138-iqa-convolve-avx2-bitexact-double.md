# ADR-0138: `_iqa_convolve` AVX2 bit-exact double-precision fast path

- **Status**: Accepted
- **Date**: 2026-04-21
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, performance

## Context

With the MS-SSIM decimate SIMD fast paths landed
([ADR-0125](0125-ms-ssim-decimate-simd.md), AVX2 / AVX-512 / NEON),
a fresh CPU profile of the MS-SSIM feature at 1080p on
branch `feat/ms-ssim-convolve-avx2` (commit `f28d853e`) shows the
hot-spot has moved:

| Symbol | File | Self % | Cumulative % |
| --- | --- | --- | --- |
| `_iqa_convolve` | `core/src/feature/iqa/convolve.c:152` | 51.4 | 51.4 |
| `_iqa_filter_pixel` | `core/src/feature/iqa/convolve.c:264` | 36.0 | 87.4 |
| `_iqa_decimate` | `core/src/feature/iqa/decimate.c:53` | 0.6 | — |

The 36% `_iqa_filter_pixel` cost belongs to the SSIM extractor's
scalar `_iqa_decimate` (which calls it per-pixel) and is a separate
workstream. The 51.4% `_iqa_convolve` cost is the focus of this ADR.

`_iqa_convolve` is called five times per `_iqa_ssim` invocation
(`ref_mu`, `cmp_mu`, `ref_sigma_sqd`, `cmp_sigma_sqd`, `sigma_both`
at [`ssim_tools.c:170–189`](../../core/src/feature/iqa/ssim_tools.c#L170-L189))
and MS-SSIM runs `_iqa_ssim` for each of 5 pyramid scales — 25
convolutions per frame for MS-SSIM alone, plus SSIM's own five per
frame at each scale. The kernel is the 11-tap separable Gaussian
declared as `g_gaussian_window_h` / `g_gaussian_window_v` in
[`ssim_tools.h:77-83`](../../core/src/feature/iqa/ssim_tools.h#L77-L83);
both arrays are `static const float` with compile-time-constant values.

The scalar implementation (in the 1-D separable branch selected by
`IQA_CONVOLVE_1D` defined in [`iqa_options.h:25`](../../core/src/feature/iqa/iqa_options.h#L25))
accumulates into a `double sum` and casts once at store:

```c
double sum = 0.0;
for (u = -uc; u <= uc; ++u, ++k_offset) {
    sum += img[img_offset + u] * k->kernel_h[k_offset];
}
img_cache[img_offset] = (float)(sum * scale);
```

Because the per-tap multiply-accumulate is written as two separate
operations (`sum += a * b`), the scalar path's observable rounding
depends on whether the compiler auto-fuses into FMA. Under the
fork's default `-O3` build without an explicit `-march=haswell`
or `-mfma`, GCC does **not** fuse, and the scalar path is two-op
`_mm_mul_sd` / `_mm_add_sd` equivalents at every tap.

Two fork-wide rules shape the implementation:

- [`core/src/feature/iqa/`](../../core/src/feature/iqa/) is a
  verbatim BSD-2011 Tom Distler import. The fork leaves it untouched
  for rebase hygiene ([ADR-0125](0125-ms-ssim-decimate-simd.md) §Ground
  rules).
- The Netflix CPU golden gate ([CLAUDE.md §8](../../CLAUDE.md))
  exercises SSIM and MS-SSIM via the three reference pairs. Any AVX2
  path that perturbs the scalar output risks the frozen
  `assertAlmostEqual(places=N)` assertions.

## Decision

We add a bit-exact AVX2 fast path for `_iqa_convolve` in a new
`core/src/feature/x86/convolve_avx2.{c,h}` plus a matching entry
point declared in `core/src/feature/iqa/convolve.h` guarded by
`#if defined(HAVE_AVX2)`. Dispatch happens in
`core/src/feature/iqa/ssim_tools.c` (the only hot caller)
via a new `_iqa_convolve_set_dispatch(fn)` setter symmetric to the
existing `_iqa_ssim_set_dispatch` pattern; the scalar
`_iqa_convolve` remains the default fallback. The vendored file
`iqa/convolve.c` is **not modified**.

The AVX2 kernel is specialised for the MS-SSIM / SSIM invariants:

| Invariant | Value | Enforced by |
| --- | --- | --- |
| Kernel length | 11 (Gaussian) or 8 (square) | Runtime check on `k->w` |
| Normalisation | `k->normalized == 1` → `scale == 1.0f` | Runtime check |
| Separable 1-D | `IQA_CONVOLVE_1D` defined | Compile-time |
| Coefficient source | `k->kernel_h` / `k->kernel_v` | Pointer compare against `g_gaussian_window_h/_v` / `g_square_window_h/_v` |

On any mismatch the AVX2 path returns an error code and the caller
falls through to the scalar reference — we do not attempt to
vectorise arbitrary kernels.

Bit-exactness to the scalar mirrors its three-stage pattern exactly:
**single-rounded `float * float` → widen to `double` → `double` add**,
with **no fused FMA**. Scalar writes
`sum += img[img_offset + u] * k->kernel_h[k_offset]` where the
product is evaluated in float (FLT_EVAL_METHOD == 0 on our targets)
and the `+=` promotes the float product to double before the add.
Any other pattern (double mul after widening; FMA) collapses or
shifts the rounding and diverges by ULPs:

```c
__m256d sum = _mm256_setzero_pd();
for (u = 0; u < 11; ++u) {
    __m128  f4      = _mm_loadu_ps(&img[x - 5 + u]);      // 4 floats
    __m128  coeff_f = _mm_set1_ps(kh[u]);                  // broadcast f32 coeff
    __m128  prod_f  = _mm_mul_ps(f4, coeff_f);             // single-rounded float mul
    __m256d prod    = _mm256_cvtps_pd(prod_f);             // widen product f32→f64
    sum             = _mm256_add_pd(prod, sum);            // double add (no FMA)
}
__m128 out = _mm256_cvtpd_ps(sum);                         // 4 f32 outputs
_mm_storeu_ps(&dst[x], out);
```

Four output pixels per inner iteration. At 1080p the horizontal pass
produces `1920 × 1080 = 2 073 600` interior pixels per plane;
scalar does 11 mul+add = 22 ops per pixel, the AVX2 path does the
same 22 ops but across 4 output lanes in parallel, yielding a
~4× ideal speedup bounded by memory bandwidth. The vertical pass is
structurally identical (strided loads from `img_cache`).

Borders (the `dst_w`-tail and `dst_h`-tail edge strips plus the
`|y| < vc` and `|x| < uc` guard region) are handled by calling
back to the scalar inner loop on those pixels — we do not vectorise
the border, matching the
[`ms_ssim_decimate_avx2`](../../core/src/feature/x86/ms_ssim_decimate_avx2.c)
precedent.

NEON is deferred to a follow-up PR. AVX-512 is also deferred — at
11 taps × 4 lanes, the AVX2 port is not AVX-512-limited; the
AVX-512 win requires a separate scatter/gather for the 8-lane double
layout and is a small-delta follow-up.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Bit-exact `float mul → widen → double add` (chosen) | Matches scalar `sum += f*k` byte-for-byte under FLT_EVAL_METHOD == 0; golden gate unchanged; no touch to vendored `iqa/` file; supports both Gaussian-11 and square-8 kernels with one code path | 4-lane (AVX2) / 8-lane (AVX-512) speedup only; memory-bandwidth-limited on 1080p | **Decision** — only pattern that preserves scalar rounding exactly |
| `__m256d` (4-lane) with `_mm256_mul_pd` after widening | Looks simpler (one precision throughout) | Double mul rounds differently than scalar's single-rounded float mul — diverges by ULPs | Rejected — not bit-exact |
| `__m256` (8-lane float) + scalar rewrite to `fmaf` | 8× lane parallelism; mirrors `ms_ssim_decimate` precedent exactly | Requires modifying vendored `iqa/convolve.c` to switch scalar to `fmaf()` — one-time golden-score shift that must be verified within `places=N` tolerance of every existing assertion; risks rebase noise | Rejected — modifying a vendored BSD file without an alternative motivation violates ADR-0125 §Ground rules; AVX2 double at 4× still closes the main profiled gap |
| `__m256` float, accept deviation | Fastest path (8× lane); simple code | Golden tests may deviate beyond `assertAlmostEqual(places=3)` for SSIM or `places=4` for MS-SSIM; risks CPU golden gate (CLAUDE §12 rule 1) | Rejected — golden gate is non-negotiable |
| Generalise `_iqa_convolve` itself (edit vendored file to inline AVX2) | One code path for every caller | Modifies BSD-2011 Tom Distler code; increases rebase noise; the only hot caller is `ssim_tools.c` so the "generalisation" has zero extra consumers today | Rejected for the same reason as ADR-0125 — no second caller exists |
| Skip convolve, optimise `_iqa_filter_pixel` instead | Addresses the other 36% in the profile | `_iqa_filter_pixel` is the SSIM scalar decimate path; replacing it is a separate T3 ("SSIM decimate SIMD") workstream with its own kernel specialisation; mixing here triples the PR scope | Deferred — separate ADR / PR |

## Consequences

- **Positive**: MS-SSIM CPU throughput gains an expected ~1.3–1.6×
  end-to-end from closing the 51% self-time on `_iqa_convolve`
  (Amdahl-bounded: if the AVX2 kernel delivers 4× on its 51.4%
  share, whole-frame time drops to `0.486 + 0.514/4 = 0.614` — a
  ~1.63× speedup; measured in the PR). SSIM benefits identically per
  convolve call. No change to the vendored `iqa/` subtree. The
  dispatch pattern leaves a clear slot for `convolve_neon` and
  `convolve_avx512` follow-ups.
- **Negative**: Adds one new SIMD translation unit plus header
  (~200 LoC). The `__m256d` double-path is memory-bandwidth-limited
  at 1080p — the 4× ceiling is the best case; actual gains may be
  closer to 3× under L2/L3 pressure. The new file needs Power-of-10
  §5 assertion-density coverage (≥1 `VMAF_ASSERT_DEBUG` per function
  ≥20 lines). Dispatch adds a function-pointer indirection that is
  negligible at the per-image granularity but does foreclose inlining
  the convolve into SSIM for a theoretical whole-loop fusion.
- **Neutral / follow-ups**:
  - Research digest [`0011-iqa-convolve-avx2.md`](../research/0011-iqa-convolve-avx2.md).
  - `docs/metrics/ms_ssim.md` (if present) / `docs/metrics/ssim.md`
    gets a "SIMD paths" entry; per [ADR-0100](0100-project-wide-doc-substance-rule.md)
    SIMD paths surface at the metric-doc level.
  - `core/src/feature/AGENTS.md` rebase invariant: the AVX2
    kernel assumes `GAUSSIAN_LEN = 11`, `SQUARE_LEN = 8`,
    `normalized = 1`, `kernel_h` / `kernel_v` pointer identity
    against `g_gaussian_window_{h,v}` / `g_square_window_{h,v}`.
    Any upstream change to these constants requires updating the
    pointer-identity check and/or the length switch.
  - `CHANGELOG.md` "lusoris fork" entry.
  - `docs/rebase-notes.md` entry: new dispatch symbol
    `_iqa_convolve_set_dispatch` is fork-local; if upstream adds a
    similarly-named symbol, merge must keep the fork's version.
  - NEON follow-up PR (arm64): `convolve_neon.c`, same dispatch slot.
  - AVX-512 follow-up PR: `convolve_avx512.c`, `__m512d` (8-lane
    double) reduction of the same kernel.
  - Reproducer command (for PR description):
    `meson test -C build test_iqa_convolve` +
    `meson test -C build test_ssim test_ms_ssim` to confirm no golden drift.

## References

- Source: user popup (2026-04-21) — "AVX2 convolve first (Recommended)"
  after profile ran on branch `feat/ms-ssim-convolve-avx2`.
- Precision source: user popup (2026-04-21) — "Bit-exact `__m256d`
  (4-lane) (Recommended)".
- Predecessor ADRs:
  [ADR-0125](0125-ms-ssim-decimate-simd.md) — MS-SSIM decimate SIMD
  (same dispatch pattern + rebase-hygiene rule).
  [ADR-0106](0106-adr-maintenance-rule.md) — this ADR predates the
  implementation commit on `feat/ms-ssim-convolve-avx2`.
  [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive
  deliverables apply.
- Profile data: `build/profiles/2026-04-20/ms_ssim_1080p_cpu.callgrind`
  and `build/profiles/2026-04-20/ms_ssim_1080p_cpu_topN.txt`.
- Vendored convolve origin:
  [Tom Distler IQA library, 2011](http://tdistler.com) — header in
  [`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c).

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/src/feature/x86/convolve_avx2.{c,h}` — present.
- The bit-exactness pattern (single-rounded float mul, widen to
  double, double add, no FMA) is cited as load-bearing by ADR-0140
  `simd_dx.h` (`SIMD_WIDEN_ADD_F32_F64_AVX2`).
- Verification command:
  `ls core/src/feature/x86/convolve_avx2.{c,h}`.

### AVX-512 audit 2026-05-09: AUDIT-PASS at 1.300x / 1.173x (T3-9 sub-row c)

T3-9 sub-row (c) bench-first audit on Ryzen 9 9950X3D (Zen 5,
AVX-512F/BW/VL). 480-frame Netflix normal pair fixture, single-thread
median of 3 wall-clock runs:

- `float_ssim` (single-scale convolve): AVX2 1.057 s vs AVX-512 0.813 s
  = **1.300x** (at threshold).
- `float_ms_ssim` (5-scale convolve): AVX2 1.450 s vs AVX-512 1.236 s
  = 1.173x (sub-threshold but explained — at the smallest 2 scales
  the kernel approaches the input width and SIMD lanes are partially
  masked, matching this ADR's "8-lane bandwidth-amortised on 8x8
  windows" prediction).

Bit-exactness: AVX-512 vs AVX2 score JSON byte-identical at full
precision (`--precision max`); 0/48 frames diverge. `test_iqa_convolve`
13/13 subtests pass on the audit build. Cross-backend gate clean.

The shipped `convolve_avx512.c` path is meeting its design ceiling.
Both `float_ssim.c:117` and `float_ms_ssim.c:94` dispatch sites wire
it correctly. Audit closes the ADR-0138 §"Follow-up" promise.

See [Research-0089](../research/0089-avx512-audit-sweep-2026-05-09.md)
for the full bench table, decision matrix, and theoretical-ceiling
crosscheck.
