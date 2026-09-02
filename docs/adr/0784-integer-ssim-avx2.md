# ADR-0784: AVX2 SIMD path for integer SSIM horizontal moment accumulation

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `simd`, `x86`, `avx2`, `ssim`, `performance`, `fork-local`

## Context

The `ssim` feature extractor (`core/src/feature/integer_ssim.c`) was entirely
scalar with no SIMD dispatch.  The per-pixel horizontal moment accumulation
in `ssim_accumulate_row()` dominates runtime: for each of the `w` output
pixels it runs a 9-tap kernel loop accumulating five int64 moments
(`mux`, `muy`, `x2`, `xy`, `y2`).  At 1080p this is roughly
`1920 x 1080 x 9 x 5` = ~93M int64 multiply-accumulate ops per frame.

PR #111 identified this as the highest-priority gap in the x86 SIMD
coverage.  The CUDA twin (`integer_ssim_score.cu`) already uses a
two-pass separable design on GPU, confirming the approach is sound.

The numerical contract (docs/principles.md §bit-exactness) requires that
SIMD output be bit-identical to scalar for integer arithmetic.

## Decision

We add an AVX2 implementation in `core/src/feature/x86/integer_ssim_avx2.c`
that processes 8 output pixels in parallel (8bpc path) or 4 output pixels
(16bpc path, which requires 64-bit products).  Runtime dispatch via
`vmaf_get_cpu_flags()` selects the AVX2 paths when `VMAF_X86_CPU_FLAG_AVX2`
is set; boundary pixels fall back to scalar within the same function.

All accumulation is performed in the same integer domain as the scalar
reference (int32 intermediates widened to int64 at store time for 8bpc;
direct int64 accumulation for 16bpc).  No floating-point is introduced.
Bit-exactness is therefore guaranteed by construction.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Vectorise the k-loop (inner) | Parallelise tap sums | k-loop is only 9 wide; short SIMD bodies; boundary handling still scalar | Chosen approach parallelises the x-loop instead — much wider |
| Vectorise both x and k loops | Maximum SIMD utilisation | Complex gather-scatter for the per-pixel offset addressing | Not needed for the projected 4-6x gain |
| AVX-512 path in addition | 16-wide x-loop for 8bpc | Requires `-mavx512bw`, adds a separate static lib carve-out | Deferred to follow-up if AVX-512 profiling shows further gain |
| Rewrite using existing float SSIM AVX2 | Reuse float_ssim kernel | Would change the output metric from integer to float moments | Ruled out: different metric, breaks bit-exact contract |

## Consequences

- **Positive**: projected 4–6x speedup on the hot path for AVX2 hosts.
  Scalar hosts (old x86, arm64, non-x86) are unaffected.
- **Positive**: dispatch is runtime-safe; no ISA gating at the call site.
- **Negative**: two new files (`integer_ssim_avx2.c`, `integer_ssim_avx2.h`)
  must be kept in sync with the scalar reference.
- **Neutral**: `integer_ssim.c` gains a function-pointer struct in
  `IntegerSsimState` (48 bytes) and two dispatch pointers in `init()`.

## References

- PR #111 (priority gap analysis, `integer_ssim` item #1).
- ADR-0139 (`ssim_avx2` bit-exact contract — double-precision reduction
  for float SSIM; not applicable here since moments are integer).
- ADR-0784 stub: `docs/adr/0784-integer-ssim-avx2.md.stub`.
- `core/test/test_integer_ssim_simd.c` — bit-exactness test.
- `docs/backends/x86/integer-ssim-avx2.md` — user-facing documentation.
