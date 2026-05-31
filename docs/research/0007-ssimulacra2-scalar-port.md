<!-- markdownlint-disable MD013 -->
# Research-0007: SSIMULACRA 2 scalar port — YUV handling, blur deviation, snapshot tooling

- **Status**: Active
- **Workstream**: ADR-0126, ADR-0130
- **Last updated**: 2026-04-20

## Question

What are the concrete engineering calls for a first-cut C port of
SSIMULACRA 2 that (a) reads YUV from `libvmaf`, not decoded RGB images;
(b) stays within the fork's "no new runtime deps" posture; and (c) lands
in a single PR small enough to review?

## Sources

- libjxl reference implementation:
  [`tools/ssimulacra2.cc`](https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc)
  (537 LOC) — the canonical algorithm.
- libjxl opsin parameters:
  [`lib/jxl/opsin_params.h`](https://github.com/libjxl/libjxl/blob/main/lib/jxl/opsin_params.h)
  — matrix coefficients `kM00`–`kM22`, bias `kB0..kB2 = 0.003793073…`.
- Python port by Pacidus:
  [PyPI `ssimulacra2==0.3.0`](https://pypi.org/project/ssimulacra2/) —
  a readable reference that replaces `FastGaussian` with
  `scipy.ndimage.gaussian_filter`, confirming that convolutional blur
  at σ=1.5, truncate=3.33 is a valid substitute when bit-exactness with
  libjxl is not a goal.
- BT.709 / BT.601 YUV matrices: ITU-R BT.709-6 §3.2 and BT.601-7 §3.
- Prior fork ADRs: [ADR-0126](../adr/0126-ssimulacra2-feature-extractor.md)
  (proposal, PR #67), [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
  (this implementation).

## Findings

1. **Colorspace ingress**. libjxl consumes sRGB-encoded PNG/JPG; libvmaf
   hands us raw YUV planes. The fork's extractor must therefore:
   a. invert the YUV matrix to reach non-linear sRGB,
   b. apply the sRGB EOTF to reach linear RGB,
   c. then follow the reference path unchanged.
   BT.709 limited-range is the default because it is the most common
   encoding for the fork's test corpus; BT.601 and full-range variants
   are surfaced as the `yuv_matrix` option. Chroma planes are upsampled
   with nearest-neighbor (1:2 shift on 4:2:0); proper chroma siting is
   a follow-up with negligible expected impact on SSIMULACRA 2 scores
   in the 0..100 range.

2. **Gaussian blur algorithm**. libjxl uses `FastGaussian` — a 3-pole
   recursive IIR approximation (Charalampidis 2016 "Recursive
   Implementation of the Gaussian Filter Using Truncated Cosine
   Functions", k={1,3,5}). An initial iteration of this port used an
   11-tap separable convolutional blur (radius=5, σ=1.5, reflect
   padding) matching the Python port's scipy-based choice; per the
   user popup "libjxl FastGaussian IIR (Recommended)" and "Bundle
   FastGaussian into this PR, SIMD follows" we replaced it with a
   bit-close scalar port of libjxl's `lib/jxl/gauss_blur.cc`. The port
   performs coefficient derivation in doubles (trig + Cramer's-rule
   3×3 solve for the β weights), stores the `n2[k]`, `d1[k]`, and
   radius in the extractor state, and applies the symmetric-sum
   recurrence `out_k = n2[k]·(in[n-N-1]+in[n+N-1]) - d1[k]·prev_k -
   prev2_k` horizontally per row and vertically per column with
   per-column IIR state. Boundaries are zero-padded (matching
   libjxl), not reflected — this is the dominant semantic difference
   from the earlier convolutional kernel and is responsible for the
   score shift observed on the 576×324 Netflix fixture (convolutional
   pipeline range 7–47 → IIR pipeline range 14–50, mean ≈24.6). For
   σ=1.5 the Python cross-check produces identical coefficients to
   the C port at 10-decimal precision (n2={0.0553, -0.0588, 0.0130},
   d1={-1.9021, -1.1756, ≈0}).

3. **Numeric types**. libjxl accumulates per-pixel SSIM/EdgeDiff
   contributions in `double` and runs the polynomial pool in `double`.
   The port preserves this — per-pixel math stays in `float` but the
   reductions and pool use `double` throughout, matching libjxl line
   for line. The 108-weight pool array is stored as `double` for the
   same reason.

4. **Memory footprint**. At 1080p, each full-resolution float RGB
   buffer is ≈24 MB. The extractor needs 10 such buffers
   (ref_lin/dist_lin/ref_xyb/dist_xyb/mu1/mu2/σ²/σ²/σ₁₂/mul) plus a
   one-plane scratch — ≈240 MB peak. Downsample reuses the `mul`
   buffer in a swap pattern to avoid a second allocation per scale.
   Geometric pyramid series keeps the total bounded at roughly 4/3 of
   full resolution across all six scales.

5. **Self-consistency test**. Feeding the same YUV as both ref and
   dist yields `100.000000` on every frame. This is a strong sanity
   check: the per-pixel numerator of SSIMMap reduces to
   `(1 - (mu-mu)²) * (2*(σ-mu²)+kC2) / ((σ-mu²)+(σ-mu²)+kC2) = 1`,
   giving `d = 0`; and the EdgeDiffMap ratio `(1+|Δ|)/(1+|Δ|) - 1 = 0`.
   All 108 weighted terms drop to zero, and the polynomial falls into
   the `ssim ≤ 0 ⇒ return 100` branch.

6. **Snapshot tooling**. `cargo install ssimulacra2_rs` failed twice
   (binary crate renamed / missing). Arch's `libjxl` package ships
   without the `tools/ssimulacra2` binary. The working path on-box is
   `pip install ssimulacra2` in a venv; the Python port operates on
   PIL-loaded PNG/JPG files, so snapshot generation will need a
   `yuv → png → score` shim. That shim plus the JSON snapshot lands
   in a follow-up PR to keep this PR focused on the C port.

## Alternatives explored

- **Separable convolutional Gaussian** — used in the first iteration
  of this port, then replaced with FastGaussian IIR per popup. Kept
  in git history for one commit before rewrite landed.
- **`cargo install ssimulacra2_rs`** for snapshot generation —
  attempted twice, crate name drift broke both invocations.
- **Build libjxl from source with `-DJPEGXL_ENABLE_TOOLS=ON`** —
  discussed but deferred; heavy dependency for a one-shot reference.
- **Match the Python port's double-precision math throughout** —
  matching `float` + `double` hybrid like libjxl proper was preferred
  for future SIMD-ification, which is easier on 32-bit lanes.
- **Cramer's rule vs. libjxl's `Inv3x3Matrix`** for the β-vector
  solve in `create_recursive_gaussian` — Cramer is simpler (~20 LOC
  of cofactor expansion) and produces bit-identical `n2`/`d1`
  coefficients at σ=1.5 vs. the libjxl matrix-inverse path. Retain
  Cramer; revisit if a future σ stresses numerical conditioning.

## Open questions

- What tolerance should the snapshot gate use? libjxl vs. the Python
  port themselves differ in the 4th decimal due to the blur kernel.
  A pragmatic starting point is `abs(score_fork - score_ref) < 0.05`
  across the Netflix fixture set; revisit once the gate is running.
- Does chroma siting matter? Nearest-neighbor upsample is the simplest
  correct option. A dedicated measurement PR could quantify the
  difference vs. proper MPEG-2 siting (0.5-pixel vertical shift for
  4:2:0).
- Is there a useful mid-precision target? SSIMULACRA 2 is not trained
  for HDR; if the fork ever runs the metric on HDR content, the
  opsin matrix and `MakePositiveXYB` constants will need re-derivation.

## Related

- ADRs: [ADR-0126](../adr/0126-ssimulacra2-feature-extractor.md),
  [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
- PRs: `#NNN` (this PR), `#67` (proposal), `#64` (unrelated, stakes ADR-0125)
