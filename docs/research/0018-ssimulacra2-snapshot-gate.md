# Research digest 0018 — SSIMULACRA 2 snapshot-JSON regression gate (T3-3)

- **Status**: Active (captures decision path for ADR-0164)
- **Related ADRs**: [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
  (deferral point), [ADR-0161](../adr/0161-ssimulacra2-simd-bitexact.md)
  / [0162](../adr/0162-ssimulacra2-iir-blur-simd.md) / [0163](../adr/0163-ssimulacra2-ptlr-simd.md)
  (the 3 SIMD ports whose output this gate pins)

## The question

ADR-0130 deferred the SSIMULACRA 2 reference-value gate with two candidate
references on the table: libjxl's `tools/ssimulacra2` and the Pacidus
Python port. Now that three SIMD ports have landed, what's the right
shape for the regression gate?

## Scoping options

| Option | Environment cost | Reference authority | Bit-exact? |
| --- | --- | --- | --- |
| libjxl `tools/ssimulacra2` | High (libjxl + PNG/JXL codec chain) | Canonical | Depends on sRGB→linear EOTF interop |
| Pacidus Python | Low (pip install) | Derived | No — scipy gaussian_filter ≠ libjxl FastGaussian IIR |
| Fork self-consistency | Zero | Fork HEAD | Yes — CPU path is bit-exact per ADR-0161/0162/0163 |

## Chosen

**Fork self-consistency gate.** Closes T3-3 at minimum scope. Catches
the practical concern: unintended behaviour change inside the fork's own
implementation (future SIMD port drift, scalar refactor breaking
libjxl-reference semantics, YUV-matrix dispatch regression, etc.).

The self-consistency story is robust because all three ISA paths
(scalar, AVX2/AVX-512, NEON) are bit-exact to each other under the
fork's ADR-0161 contract. Any pinned value is reproducible on any
platform.

## What the test checks

Two YUV fixtures, both already checked in under
`python/test/resource/yuv/`:

1. `src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv` (576×324, 48f):
   canonical Netflix test pair, covers 6-scale pyramid fully.
2. `ref_test_0_1..._q_160x90.yuv` vs `dis_test_...` (160×90, 48f):
   small fixture, exercises the sub-176 path where the pyramid
   terminates early (fewer than 6 scales).

Per fixture: pooled `mean` / `min` / `max` / `harmonic_mean` + frame 0

- frame 47. 6 asserts × 2 fixtures = 12 asserts. Tolerance: 4 decimal
places (well within the ADR-0161 bit-exact contract's 0 ulp drift).

## Why not cross-check against libjxl

- `ssimulacra2_rs` cargo install is currently broken (ADR-0130 §Context).
- Building `tools/ssimulacra2` from libjxl source in CI adds: git clone
  libjxl + build deps (libhwy, libbrotlidec, skcms, libjpeg-turbo) + PNG
  codec + JXL codec + CMake build + bin install. Significant scope.
- The fork tracks libjxl's algorithm precisely (the scalar port IS a
  line-for-line C translation of `tools/ssimulacra2.cc`); internal
  self-consistency is a strong proxy for external consistency.

## Why not Pacidus Python

- Python port uses `scipy.ndimage.gaussian_filter` (separable 1D
  convolution with reflect-pad).
- libjxl (and our fork) uses `FastGaussian` 3-pole IIR (Charalampidis
  2016) with zero-pad.
- These disagree by a bounded-but-nonzero amount on every frame. A
  Pacidus-comparison gate would need a hand-tuned tolerance, which
  weakens the signal.

## Follow-ups

- If `ssimulacra2_rs` becomes installable again, add a second cross-
  reference gate in a follow-up PR.
- If a CUDA/SYCL SSIMULACRA 2 port is added (currently neither exists),
  the test needs `--no_cuda --no_sycl` flags or a split variant.
- Consider adding the 160×90 fixture's 8-bit YUV scores to
  `testdata/scores_cpu_ssimulacra2.json` for an eventual unified
  CPU-golden snapshot format alignment.
