<!-- markdownlint-disable MD013 MD060 -->
# ADR-0130: SSIMULACRA 2 scalar implementation

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: lusoris
- **Tags**: `metrics`, `feature-extractor`, `ssimulacra2`

## Context

ADR-0126 (Proposed in PR #67) scopes the SSIMULACRA 2 workstream at the
proposal level — choosing the metric and its place in the fork's feature
surface. This ADR closes out the scalar-port implementation: the concrete
C sources, the color-space handling, the Gaussian-blur algorithm, and
the scope split between the scalar baseline (this PR) and the SIMD
variants (follow-ups).

## Decision

We ship a scalar-only `vmaf_fex_ssimulacra2` feature extractor in
[`core/src/feature/ssimulacra2.c`](../../core/src/feature/ssimulacra2.c)
that:

1. Ingests YUV 4:2:0 / 4:2:2 / 4:4:4 at 8/10/12 bpc with nearest-neighbor
   chroma upsampling, converts to non-linear sRGB via a configurable
   YUV→RGB matrix (`yuv_matrix` option: BT.709/BT.601 × limited/full),
   then applies the sRGB EOTF to reach linear RGB.
2. Converts linear RGB → XYB using libjxl's exact opsin absorbance
   matrix and cube-root bias, then applies `MakePositiveXYB`.
3. Computes six pyramid scales with 2×2 box downsampling in linear RGB
   between scales, per-scale Gaussian blur via libjxl's `FastGaussian`
   3-pole recursive IIR (k={1,3,5}, Charalampidis 2016 truncated-cosine
   approximation, zero-pad boundaries — bit-close port of
   `lib/jxl/gauss_blur.cc`), per-scale `SSIMMap` and `EdgeDiffMap`,
   and the final 108-weight polynomial pool with the canonical libjxl
   coefficients.
4. Exposes one feature, `ssimulacra2`, in the 0..100 range with
   identity inputs returning exactly `100.000000`.

Snapshot-comparison against `tools/ssimulacra2` ships as a follow-up PR
(`ssimulacra2_rs` cargo install currently broken; Pacidus Python port
uses scipy's convolutional Gaussian and so cannot verify the IIR port).
This PR does not commit `testdata/scores_cpu_ssimulacra2.json`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Scalar port + libjxl FastGaussian IIR (chosen)** | Algorithmically matches libjxl's canonical pipeline; ~2× faster than a 11-tap convolutional kernel at σ=1.5; independent of kernel-radius tuning; clear SIMD path (libjxl's own 4-lane unroll) for follow-up PRs | Adds ~120 LOC of coefficient derivation (Cramer's rule 3×3 solve + trig) and per-column state for the vertical pass | Chosen: the popup explicitly picked "libjxl FastGaussian IIR (Recommended)" as the blur algorithm |
| Scalar port + separable convolutional Gaussian | Simpler (~40 LOC); matches scipy's `gaussian_filter` used by the Pacidus Python port | Drifts from libjxl's canonical output; reflect-pad semantics differ; scores diverge from `tools/ssimulacra2` | Rejected per popup — the fork standardises on the libjxl reference, not the Python reference |
| Link against system libjxl for the whole inner loop | Zero algorithmic drift | Hard dependency on libjxl 0.11+ headers; pulls in CMS + image-bundle + 10+ other headers; violates the fork's "no new runtime deps" posture | Rejected — too invasive for a metric that is ~650 LOC of well-understood math |
| Ship scaffold only, defer implementation to N PRs | Smaller PR surface | User explicitly picked "Full scalar port in one PR" in the pre-work popup | Rejected per direct user direction |

## Consequences

- **Positive**: `ssimulacra2` joins the fork's feature surface as a
  runnable CPU metric with libjxl-equivalent blur semantics; SIMD
  follow-ups can mirror libjxl's 4-lane unroll one-for-one.
- **Positive**: IIR runtime is independent of σ — future research on
  alternative blur kernels does not need to rebuild the radius/truncate
  constants.
- **Negative**: scalar path is ~1 fps at 1080p — unsuitable for
  interactive use until the AVX2/AVX-512/NEON variants land. The IIR
  recurrence is data-dependent, so speedups come from per-lane unroll,
  not from widening a single kernel tap.
- **Negative**: coefficient derivation in `create_recursive_gaussian`
  runs in doubles with Cramer's rule and may differ from libjxl's
  `Inv3x3Matrix` in the ULP of the stored `n2`/`d1` floats. For σ=1.5
  both implementations produce identical coefficients when printed to
  10 decimals; bit-exactness of the per-frame score depends on the
  coefficient path staying stable. The AVX2 SIMD follow-up will use
  the same scalar coefficient path, ensuring bit-exactness across
  backends.
- **Neutral / follow-ups**:
  - PR N+1: AVX2 SIMD variant for the per-plane inner loops
    (FastGaussian 4-lane unroll, XYB matrix mul, SSIM/EdgeDiff maps).
  - PR N+2: AVX-512 + NEON variants.
  - PR N+3: snapshot JSON via `tools/ssimulacra2` once the cargo
    install is unblocked, gated in CI at a documented tolerance.
  - PR N+4: optional CUDA/SYCL backend once the scalar path is stable.

## References

- libjxl algorithm: [`tools/ssimulacra2.cc`](https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc)
- libjxl FastGaussian IIR: [`lib/jxl/gauss_blur.cc`](https://github.com/libjxl/libjxl/blob/main/lib/jxl/gauss_blur.cc)
- Charalampidis 2016: "Recursive Implementation of the Gaussian Filter
  Using Truncated Cosine Functions"
- Python reference: [Pacidus/ssimulacra2](https://pypi.org/project/ssimulacra2/)
- Proposal ADR: [ADR-0126](0126-ssimulacra2-feature-extractor.md) (Proposed in PR #67)
- Related research: [Research-0007](../research/0007-ssimulacra2-scalar-port.md)
- Source: `req` — user popup answers "Full scalar port in one PR
  (Recommended)" + "Bundle FastGaussian into this PR, SIMD follows" +
  "libjxl FastGaussian IIR (Recommended)"
