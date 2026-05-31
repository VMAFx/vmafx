<!-- markdownlint-disable MD056 MD060 -->
# Research-0003: SSIMULACRA 2 port sourcing and upstream-drift strategy

- **Status**: Active
- **Workstream**: [ADR-0126](../adr/0126-ssimulacra2-extractor.md)
- **Last updated**: 2026-04-20

## Question

Of the several available SSIMULACRA 2 implementations, which one do
we port into the fork as the authoritative reference, and how do we
track the chosen upstream over time so post-port bug-fixes propagate
without a full re-port?

## Sources

- [`libjxl/tools/ssimulacra2.cc`](https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc)
  — the reference the JPEG XL team considers authoritative since the
  2022 merge. BSD-3-Clause. ~800 lines C++ plus ~200 lines of header.
- [`cloudinary/ssimulacra2`](https://github.com/cloudinary/ssimulacra2)
  — the original Cloudinary research release that predates the libjxl
  merge. Archived / read-only upstream. Identical algorithm, older
  code layout.
- [`rust-av/ssimulacra2`](https://crates.io/crates/ssimulacra2) —
  Rust port by the rust-av group, used by `ssimulacra2_bin`.
  Actively maintained; MIT-licensed.
- [`psy-ex/metrics`](https://github.com/psy-ex/metrics) — C++
  library by the psy-ex / aomenc communities bundling SSIMULACRA 2
  alongside XPSNR, Butteraugli, etc. LGPL.
- [Alakuijala et al., SSIMULACRA 2, arXiv:2309.02960](https://arxiv.org/abs/2309.02960)
  — the paper.
- [libjxl ADR on SSIMULACRA 2 merge](https://github.com/libjxl/libjxl/pull/1482)
  — documents the libjxl adoption and the decision to make it the
  canonical reference.

## Findings

### Algorithm shape (shared across all four implementations)

1. Convert both reference and distorted images from RGB (or YUV → RGB
   via BT.601 / BT.709 matrix) into the **XYB** colour space (the
   JPEG XL perceptual space — roughly an LMS opponent-colour space
   with a cube-root non-linearity).
2. Produce a Gaussian pyramid of six scales on each XYB plane
   (standard 5-tap separable blur, decimate by 2).
3. At each scale, compute an SSIM-like similarity — but with an
   **asymmetric error term**: negative errors (loss of texture) are
   weighted ~4× more heavily than positive errors (added noise) in
   the aggregation.
4. Compute a per-plane-per-scale score; pool via a trained
   least-squares fit (coefficients published in the libjxl source)
   into a single 0–100 score (100 = pristine).

The per-pixel work is dominated by the XYB conversion and the
Gaussian pyramid. Cost estimate: **~2–3× VMAF** per frame on a single
CPU thread, before SIMD. After SIMD this should settle to ~1× VMAF,
in line with the Fraunhofer XPSNR characterisation.

### License compatibility

| Source | License | Compat with our BSD-3-Clause-Plus-Patent | Notes |
|---|---|---|---|
| libjxl | BSD-3-Clause | Yes | Can copy verbatim with attribution |
| cloudinary | Apache 2.0 | Yes | Archived; no updates expected |
| rust-av | MIT | Yes, but brings Rust build-dep into the matrix — not viable here |
| psy-ex | LGPL-2.1 | Problematic for our BSD-plus-Patent shipping licence; copyleft collides with the fork's static-linking and GPU-backend story |

### Port cost (rough)

- XYB colour-space conversion: ~80 LOC — trivial port, well-defined
  constants.
- Gaussian pyramid: ~60 LOC + dispatch. Can later share its
  separable-SIMD scaffold with MS-SSIM decimate's post-ADR-0125
  infrastructure.
- Asymmetric similarity: ~120 LOC per plane.
- Pool + LSQ: ~40 LOC; coefficients copied verbatim from libjxl.
- Total new C (scalar): ~300–400 LOC + header.

This is strictly smaller than CAMBI (which we already ship) and
comparable in size to the integer VIF extractor.

### Upstream drift

libjxl is in maintenance mode for SSIMULACRA 2 specifically — the
algorithm is considered stable and the coefficients are frozen. What
*has* changed in libjxl since the 2022 merge is (a) moving the XYB
constants behind a shared header, and (b) switching the Gaussian
separable coefficients to a highway-SIMD variant. Neither change
alters the numerical output — both are refactors.

Our drift-tracking proposal: **pin to a specific libjxl commit** in
the ported file's header comment. On every fork `sync-upstream`
run, inspect the libjxl repo at HEAD for any commits touching
`tools/ssimulacra2.cc` since our pin; file an issue listing them for
manual review. This is strictly lighter than tracking Netflix VMAF
upstream and is expected to fire ~0–2 times per year.

## Answered questions (for the ADR)

- **Which port source?** libjxl C++ reference (ADR-0126 decision).
- **Which license flag goes in the ported header?** The libvmaf
  `Copyright 2026 Lusoris and Claude (Anthropic)` header on top +
  a second block acknowledging libjxl BSD-3-Clause, per
  [ADR-0105](../adr/0105-copyright-handling-dual-notice.md).
- **Do we bit-equal the reference?** No. Bit-closeness within a
  documented float tolerance (target: relative error < 1e-5 on the
  pooled score, which is approximately "last-ULP-or-two"). Exact
  equality is impractical because libjxl uses `-ffast-math` +
  highway-SIMD reductions that aren't achievable from scalar C
  without reimplementing the same reductions.

## Open questions (for follow-up iterations)

- **Should we expose the per-scale / per-plane sub-scores as
  separate VMAF features?** The libjxl CLI exposes a single score;
  the per-component internals might be useful for debug-mode output.
  Defer until the scalar extractor lands and we see if users ask.
- **Colour-space input contract**: libjxl accepts sRGB float; our
  pipeline hands luma-only YUV by default. The chroma-weighted XYB
  path means we need the full YUV → RGB → XYB conversion at the
  extractor entry point. Confirm the BT.709 / BT.2020 matrix choice
  matches the VMAF `enable_transform` convention in
  [`core/src/feature/vif.c`](../../core/src/feature/vif.c).
- **SIMD speedup ceiling**: libjxl's highway backend reports ~6×
  AVX2 vs scalar for the Gaussian-pyramid stage; XYB conversion is
  element-wise and vectorises ~linearly. Budget the follow-up SIMD
  ADR around a 4–6× AVX2 / 8–12× AVX-512 target.

## Next steps

1. Scalar C port lands under `core/src/feature/ssimulacra2.c`
   (workstream-opened PR after governance merges).
2. Snapshot `testdata/scores_cpu_ssimulacra2.json` generated via
   [`/regen-snapshots`](../../.claude/skills/regen-snapshots/SKILL.md)
   from the libjxl CLI on the three Netflix golden pairs and one
   AV1-low-bitrate fixture from the fork's test bench.
3. Doc entry in [`docs/metrics/features.md`](../metrics/features.md)
   extends the "full-reference" section with the new metric.
4. SIMD follow-up ADR + implementation after scalar lands and has at
   least two weeks of cross-backend soak in CI.
