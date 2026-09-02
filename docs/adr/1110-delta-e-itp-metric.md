<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1110: Add ΔE-ITP (Delta E ITP) — PQ-only HDR colour-difference CPU extractor

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: metric, feature-extractor, hdr, colour, fork-local

## Context

The fork measures SDR colour difference (`ciede`, ΔE2000, hardcoded BT.709)
and perceptual structure (`ssimulacra2`), but has **no HDR / wide-colour-gamut
colour-difference metric**. ΔE-ITP (ITU-R BT.2124-0) is the ITU-standardised
metric for assessing the visibility of colour differences in PQ and HLG HDR
signals; adding it closes the WCG/HDR colour-fidelity gap.

ΔE-ITP is a fully analytic per-pixel metric (no trained model). It maps
cleanly onto the existing `VmafFeatureExtractor` interface and mirrors the
structure of `ciede.c` (per-pixel kernel, chroma upsampling, 8/16-bit reads,
double-precision frame sum, mean pooling).

Two forces constrain the scope. First, `VmafPicture` carries **no
transfer-function or colorimetry metadata**, so the extractor must *assume* a
transfer function and primaries, exposed as options. Second, an adversarial
verification of the dossier
([`.workingdir2/rc/metrics/deltae-itp.md`](../../.workingdir2/rc/metrics/deltae-itp.md))
confirmed that the PQ pipeline — every matrix, the five PQ constants, the ITP
scaling, the ×720 factor — is triple-sourced against the normative ITU-R
BT.2124-0 PDF plus independent references, whereas the HLG (Annex 3) and
BT.1886/SDR (Conversion 5) paths are **single-sourced** in BT.2124-0 itself
and were not independently cross-validated.

## Decision

We will add a CPU-only feature extractor `delta_e_itp` (provided feature key
`delta_e_itp`) implementing the ITU-R BT.2124-0 Annex 1 ΔE-ITP pipeline for
the **PQ (SMPTE ST-2084) transfer function only**. It exposes three string
options — `transfer` (default `pq`; only `pq` accepted this release),
`matrix` (`bt2020` default / `bt709`), and `range` (`limited` default /
`full`) — pools as the mean per-pixel ΔE-ITP, rejects 4:0:0 input with
`-EINVAL`, and does all per-pixel math in double precision without clamping
out-of-gamut LMS/ICtCp values. The HLG and BT.1886/SDR transfers are deferred
follow-ups and are rejected with a clear error until their constants are
independently validated.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **PQ-only ΔE-ITP now, defer HLG/SDR** (chosen) | Every shipped constant is double/triple-confirmed; smallest correct surface; matches `ciede`/`ssimulacra2` CPU-first staging | No HLG/SDR colour difference yet | Best risk/value: ships the verified HDR core without unvalidated constants |
| Ship all three transfers (PQ + HLG + BT.1886) now | One-shot complete BT.2124-0 coverage | HLG OOTF constants and the BT.709→BT.2100 primaries are single-sourced; risk of shipping an unverified number | Rejected — violates the fork's "every constant double-confirmed" bar |
| Do nothing / rely on `ciede` for HDR | No new code | `ciede` is BT.709 SDR; produces meaningless numbers on PQ HDR | Rejected — leaves the WCG/HDR gap open |
| Refuse to run without explicit `transfer` (no default) | No silent SDR-as-PQ mistakes | Worse UX; every invocation needs the flag; `VmafPicture` lacks the metadata to validate anyway | Rejected — `transfer=pq` default with a loud doc warning is the lighter contract |

## Consequences

- **Positive**: the fork gains a standards-compliant HDR/WCG colour-difference
  metric; the per-pixel ITP triple is asserted against the BT.2124-0 normative
  oracle at places=4; no new dependencies or model assets.
- **Negative**: PQ-only — HLG and SDR HDR content are out of scope until a
  follow-up validates those constants. Feeding non-PQ content to the metric
  produces meaningless numbers (documented).
- **Neutral / follow-ups**: SIMD twins, GPU twins (target places=4–5, not
  bit-exact, per ADR-0220), a committed deterministic PQ LUT for byte-exact
  cross-host reproducibility, and the HLG / BT.1886 transfer options are all
  deferred follow-ups. The pre-existing stale `feature_extractor.c` (dead
  twin of the live C++23 `feature_extractor.cpp`, ADR-0846) is a maintenance
  trap surfaced during this work and should be removed separately.

## References

- ITU-R BT.2124-0 (01/2019), *Objective metric for the assessment of the
  potential visibility of colour differences in television* — Annex 1
  (normative pipeline: RGB→LMS, PQ EOTF, LMS'→ICtCp, ITP scaling,
  ΔE_ITP = 720·√Σ), Annex 4 (normative worked example: 58 % PQ blue
  ITP = [0.3554, 0.1346, -0.1613], ΔE = 2.363).
- ITU-R BT.2100 — RGB↔LMS integer matrix, PQ (ST-2084) transfer function,
  ICtCp definition.
- SMPTE ST 2084:2014 — PQ EOTF.
- Verified design dossier:
  [`.workingdir2/rc/metrics/deltae-itp.md`](../../.workingdir2/rc/metrics/deltae-itp.md)
  (adversarial verdict: GO-WITH-FIXES; all constants triple-sourced; required
  fixes — assert the full-precision ITP triple at places=4 instead of the
  pre-rounded 2.363, and ship PQ-only — applied).
- In-tree pattern sources: `core/src/feature/ciede.c` (closest analog),
  `core/src/feature/ssimulacra2_math.h` (deterministic math-helper header
  pattern).
- Source: `req` — maintainer task brief specifying PQ-only RC scope, feature
  name `delta_e_itp`, mean pooling, `matrix`/`range` options defaulting to
  bt2020/limited, double-precision math, no out-of-gamut clamping, and the
  places=4 ITP-triple oracle.
