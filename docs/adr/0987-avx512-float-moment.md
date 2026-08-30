# ADR-0987: AVX-512 path for float_moment feature extractor

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `simd`, `avx512`, `performance`, `float_moment`, `fork-local`

## Context

The `.workingdir2` SIMD-coverage audit identified `float_moment` as one of
three x86 features still lacking an AVX-512 implementation.  The AVX2 path
(`moment_avx2.c`, ADR-0179) processes 8 floats per iteration; on CPUs that
expose `VMAF_X86_CPU_FLAG_AVX512` the 8-wide inner loop leaves half of the
512-bit ZMM register unused.

`float_moment` computes two simple reductions per frame (first and second
statistical moment over a float-valued picture).  It is a pure reduction
with no inter-pixel dependence, making it an ideal candidate for lane
widening: doubling from 8 to 16 floats per loop body halves the number of
main-loop iterations and allows the CPU to issue the load + multiply + store
sequence without pipeline stalls.

The feature was wired with an AVX2 gate in `float_moment.c` but no AVX-512
branch.  This ADR closes that gap.

## Decision

We will add `core/src/feature/x86/moment_avx512.c` and the corresponding
header, following the same sequential-lane-widening accumulation pattern
established by `float_psnr_avx512.c`: load 16 floats into a ZMM, store to
a 64-byte aligned temporary, then add each lane sequentially into a `double`
accumulator.  The dispatch in `float_moment.c` is extended with a
`HAVE_AVX512` guard that selects the 16-lane path when
`VMAF_X86_CPU_FLAG_AVX512` is set, overriding the AVX2 selection.

The existing `test_moment_simd.c` is extended with four AVX-512 parity
cases that compare `compute_1st/2nd_moment_avx512` against their scalar
references using the same `MOMENT_REL_TOL = 1e-7` relative tolerance as the
AVX2 tests.  The cases run conditionally behind `simd_test_have_avx512()` so
they skip cleanly on hosts without AVX-512.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Horizontal `_mm512_reduce_add_ps` for the full row at once | Fewer store instructions; may hide more ILP from the OOO engine | Produces a different reduction order than the scalar reference, breaking the `MOMENT_REL_TOL` contract and requiring a separate tolerance justification | Tolerance-bounded-but-not-bit-exact is the documented contract (ADR-0179); the sequential lane-widening pattern used by `float_psnr_avx512.c` stays within the existing contract without introducing a new gate |
| Separate `test_moment_avx512_parity` test file (new file, mirroring `test_motion_avx512_parity.c`) | Matches the motion test's file-per-ISA layout | The existing `test_moment_simd.c` already covers both x86 and AARCH64 under a single file; adding four cases to it is less disruptive and matches the AVX2 addition pattern from ADR-0179 | Simpler; no new executable to wire into `meson.build` |
| Skip the AVX-512 path entirely (leave AVX2 as the ceiling) | No new code | Leaves a known coverage gap on all ICX/SPR/SRF CPUs; contradicts the `.workingdir2` SIMD audit directive | Gap closure is the explicit driver for this PR |

## Consequences

- **Positive**: `float_moment` on AVX-512 CPUs processes 16 floats per
  inner-loop iteration instead of 8, reducing loop overhead approximately
  in half for HD/UHD frames.  The dispatch layer transparently selects the
  best available path at runtime, so no user-visible API change is needed.
- **Negative**: One additional `.c` / `.h` file pair in
  `core/src/feature/x86/`, adding minor build-time overhead.  The
  `x86_avx512_static_lib` grows by one TU.
- **Neutral / follow-ups**:
  - `psnr_hvs` and `ssimulacra2_host` were identified in the same SIMD
    audit; they are deferred to separate PRs (each requires its own AVX-512
    lane-width analysis and tolerance-bound justification).
  - The `test_moment_simd` executable gains four new test cases; CI
    wall-time impact is negligible (pure arithmetic, no I/O).

## References

- ADR-0179: float_moment AVX2 path (original coverage)
- ADR-0245: `simd_bitexact_test.h` shared harness
- ADR-0854: `simd_test_have_avx512()` addition to the shared harness
- `float_psnr_avx512.c`: precedent for sequential-lane accumulation pattern
- `.workingdir2` SIMD-coverage audit: three x86 gaps identified
- req: user directive — implement the AVX-512 path for `float_moment`
