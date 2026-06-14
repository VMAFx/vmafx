<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1111: Add PU21 HDR perceptual metric (PU-PSNR + PU-SSIM, PQ input only)

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: feature-extractor, hdr, pu21, metric, fork-local

## Context

Every full-reference metric in the fork (PSNR, SSIM, MS-SSIM, CIEDE2000,
SSIMULACRA2, CAMBI, PSNR-HVS) operates on gamma- or limited-range YUV and is
not perceptually valid on absolute-luminance HDR content. PSNR/SSIM computed
directly on PQ code values or on linear HDR correlate poorly with perception
above ~100 nit. PU21 (Mantiuk & Azimi, QoMEX/PCS 2021) closes that gap with a
single closed-form, contrast-sensitivity-fitted transfer curve: it maps
absolute display luminance (cd/m², up to 10000) onto a perceptually uniform
scale on which the existing, already-validated SDR metrics become meaningful.
This is the standard, peer-reviewed "transform-then-score" HDR adapter, and the
closest analog to the existing CIEDE / SSIMULACRA2 extractors.

The fork had no HDR perceptual adapter. The verified design dossier
(`.workingdir2/rc/metrics/pu21.md`; all 28 coefficients byte-verified against
two independent authoritative sources, the encoder + PU-PSNR oracle
re-derived in fp64) confirmed the math is RC-ready; the remaining work is the
integration and scope decisions captured here.

## Decision

We will add a CPU feature extractor `pu21` providing two features,
`pu21_psnr` and `pu21_ssim`. It encodes the **luma (Y) plane only** through the
PU21 transfer function (canonical `banding_glare` variant; all four variants
selectable via a `variant` option), then computes PSNR (peak = 256, **no** SDR
dB cap) and a self-contained single-scale Gaussian SSIM at data range L = 256
on the encoded planes. Input is **PQ (SMPTE ST.2084) only** for RC: the integer
luma code value is decoded to absolute cd/m² via the ST.2084 EOTF × 10000, then
clamped to [0.005, 10000] and PU21-encoded. A `transfer` option defaults to
`pq` and rejects every other value with `-EINVAL` (HLG / SDR-gamma deferred,
consistent with the ΔE-ITP HDR-scope decision). All per-pixel math is double
precision.

PU-SSIM requires SSIM at L = 256 (the reference `pu21_metric.m` uses
`ssim(...,'DynamicRange',256)`), but the fork's shared `iqa_ssim()` hardcodes
L = 255 and feeds the Netflix golden assertions. We therefore implement a
**separate, self-contained L-parameterised SSIM** (`pu21_ssim.c`) rather than
modifying or parameterising the golden SSIM. The golden SSIM stays byte-identical
and untouched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **PU-PSNR + PU-SSIM, PQ-only, luma-only (chosen)** | Both standard PU metrics; matches SSIM's luminance path; PQ is the common HDR case; smallest correct surface | Defers HLG/SDR and chroma | Best correctness-per-LOC; HLG/SDR are free design params with no pu21-source authority |
| PU-PSNR only | Smaller surface | Drops PU-SSIM, the more perceptual of the two; pu21_metric.m ships both | Under-delivers the dossier's "both" recommendation |
| Modify/parameterise the shared `float_ssim` / `iqa_ssim` to take a data range | No duplicate SSIM code | `iqa_ssim` feeds the Netflix **golden** assertions (L=255 hardcoded); any change risks the protected golden gate | **Rejected** — golden SSIM must remain byte-identical |
| Defer PU21 entirely | No work | Leaves the HDR perceptual gap open | The math is verified RC-ready; deferral has no upside |
| Per-channel (Y/Cb/Cr) encoding for PSNR | Closer to pu21_metric per-channel PSNR | Chroma-in-PU is ambiguous; SSIM path is luminance-only anyway | Luma-only is the simplest correct default; chroma is a later scope decision |

## Consequences

- **Positive**: the fork gains a peer-reviewed HDR perceptual adapter; reuses
  the read-only iqa Gaussian-convolve helper; the golden SSIM is untouched; the
  encoder is exercised exactly against the fp64 oracle (`test_pu21`, places=4).
- **Negative**: a second small SSIM implementation now exists (the
  L-parameterised `pu21_ssim.c`) alongside `iqa_ssim`; it is deliberately
  scalar and single-scale.
- **Neutral / follow-ups**: HLG (needs a `peak_luminance` OOTF option) and
  SDR-gamma (`multiplier`) input paths, chroma encoding, PU-MS-SSIM, and GPU /
  SIMD twins are out of scope for RC and can be added on top of this ADR.

## References

- Design dossier (verified): `.workingdir2/rc/metrics/pu21.md` — all 28
  coefficients byte-verified; encoder re-derived to ~1e-11; PU-PSNR(100,99)
  oracle = 51.87333880351542 dB reproduced exactly in fp64.
- Mantiuk & Azimi, "PU21: A novel perceptually uniform encoding for adapting
  existing quality metrics for HDR", QoMEX/PCS 2021 —
  <https://ieeexplore.ieee.org/document/9477471/>.
- Official reference (encoder coefficients + formula + range + default variant):
  <https://github.com/gfxdisp/pu21> (`matlab/pu21_encoder.m`, BSD 3-Clause,
  University of Cambridge 2021); downstream peak=256 / SSIM DynamicRange=256 from
  `pu21_metric.m`.
- Independent cross-check of all 28 coefficients + formula + default variant
  (`banding_glare`) + multiplier default: the FFmpeg pu21 filter C port.
- SMPTE ST.2084 (PQ) EOTF constants — standard, double-confirmed.
- Golden-SSIM constraint: `core/src/feature/iqa/ssim_tools.c` (`int L = 255`)
  feeds the protected Netflix assertions and must not change.
