<!-- markdownlint-disable MD060 -->
# ΔE-ITP (Delta E ITP) — HDR/WCG Colour-Difference Feature Extractor

**Feature name:** `delta_e_itp`
**Provided feature key:** `delta_e_itp`
**ADR:** [ADR-1110](../adr/1110-delta-e-itp-metric.md)
**References:**

- ITU-R BT.2124-0 (01/2019), *Objective metric for the assessment of the
  potential visibility of colour differences in television* — Annex 1
  (normative pipeline), Annex 4 (normative worked example).
- ITU-R BT.2100, *Image parameter values for high dynamic range television*
  — RGB↔LMS matrix, PQ (ST-2084) transfer function, ICtCp definition.
- SMPTE ST 2084:2014 — Perceptual Quantizer (PQ) EOTF.

## Overview

`delta_e_itp` is a full-reference, per-pixel colour-difference metric for
**HDR / wide-colour-gamut** video, defined by ITU-R BT.2124-0. It measures
the perceptual visibility of colour differences between a reference and a
distorted frame in the **ITP** colour space (a scaled form of ICtCp).

A ΔE-ITP value of approximately **1.0 corresponds to one just-noticeable
colour difference** under the most critical adaptation state. Higher values
mean a larger (more visible) colour error. An identical reference/distorted
pair yields exactly `0.0`.

This fills the fork's HDR/WCG colour-fidelity gap: `ciede` (ΔE2000) is an
SDR / BT.709 metric, and `ssimulacra2` is a perceptual structural metric;
neither is appropriate for measuring colour error on PQ HDR content.

## What it measures

Per pixel, both frames are converted to the ITP colour space and the
scaled Euclidean distance is taken:

```text
ΔE_ITP = 720 * sqrt( (ΔI)^2 + (ΔT)^2 + (ΔP)^2 )
```

The per-frame score is the **mean per-pixel ΔE-ITP** over the frame
(`sum / (width * height)`).

## Pipeline (ITU-R BT.2124-0 Annex 1)

For each pixel of both reference and distorted frames:

1. **YUV → non-linear R'G'B' [0, 1]** — dequantize by bit depth using the
   configured quantization `range` and apply the inverse `matrix`.
2. **PQ EOTF** — decode the non-linear PQ signal to linear,
   display-referred RGB (0–10 000 cd/m²).
3. **RGB → LMS** — BT.2100 integer matrix (rows ÷ 4096).
4. **PQ inverse EOTF** — re-encode LMS luminance to the non-linear L'M'S'
   domain that ICtCp is defined on.
5. **L'M'S' → ICtCp** — BT.2100 integer matrix (rows ÷ 4096).
6. **ITP scaling** — `I = I`, `T = 0.5·Ct`, `P = Cp`.

All per-pixel math is done in **double precision** (matching the `ciede`
precedent). Out-of-gamut negative LMS / ICtCp values are **not clamped**,
so out-of-gamut colour errors remain measurable (BT.2124 Annex 4).

Chroma planes of 4:2:0 / 4:2:2 input are upsampled to luma resolution
before conversion; 4:4:4 input is used directly. **4:0:0 (luma-only) input
is rejected** with an error, since a colour-difference metric requires
chroma.

## Scope: PQ transfer only (this release)

> **Important:** This release implements the **PQ (SMPTE ST-2084)** transfer
> function **only**.

`VmafPicture` carries no transfer-function or colorimetry metadata, so the
extractor must *assume* a transfer function. The default — and the only
accepted value in this release — is `transfer=pq`. **Feeding non-PQ content
(SDR, HLG) to this metric produces meaningless numbers.**

The HLG and BT.1886/SDR transfer paths defined in BT.2124-0 (Annex 3 /
Conversion 5) are **deferred follow-ups**: their constants are
single-sourced in the standard and were not independently cross-validated,
so they are intentionally not shipped here. Requesting `transfer=hlg` or
`transfer=bt1886` returns an error at initialisation. See
[ADR-1110](../adr/1110-delta-e-itp-metric.md) for the rationale.

## Options

| Option     | Values                | Default     | Meaning |
|------------|-----------------------|-------------|---------|
| `transfer` | `pq`                  | `pq`        | Assumed EOTF. Only `pq` is accepted in this release (HLG / BT.1886 deferred). |
| `matrix`   | `bt2020`, `bt709`     | `bt2020`    | YUV → R'G'B' colour matrix coefficients. `bt2020` (NCL) is the BT.2100 / HDR norm; `bt709` is provided for SDR-on-BT.2100 experimentation. |
| `range`    | `limited`, `full`     | `limited`   | YUV quantization range. `limited` (narrow/video range) is the broadcast norm; `full` uses the entire code-value range. |

## Usage

```bash
# Default: PQ transfer, BT.2020 NCL matrix, limited (narrow) range.
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 10 \
     --feature delta_e_itp --output out.xml
```

```bash
# With explicit options. The CLI feature-option syntax is
# `name=key=val:key2=val2` (first '=' separates the name; ':' separates
# subsequent key=value pairs).
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 444 --bitdepth 10 \
     --feature "delta_e_itp=matrix=bt2020:range=full" --output out.xml
```

No build flag is required: `delta_e_itp` is compiled unconditionally.

## Interpreting the score

- `0.0` — reference and distorted frames are identical.
- `~1.0` — about one just-noticeable colour difference (the threshold of
  visibility under critical viewing).
- larger — proportionally larger, more visible colour error.

## Implementation notes

- **CPU scalar only** for now. AVX2 / AVX-512 / NEON SIMD twins and
  CUDA / SYCL / HIP / Metal GPU twins are out of scope for this release
  (the same staging `ciede` and `ssimulacra2` followed). A future GPU twin
  computing the PQ EOTF in fp32 should target a places=4–5 parity bound,
  not bit-exactness (ADR-0214 / ADR-0220).
- **No model assets.** ΔE-ITP is a fully analytic, standardised metric with
  no trained weights or lookup tables. The constants come directly from
  ITU-R BT.2124-0 / BT.2100.
- **PQ transfer functions** live in
  [`core/src/feature/delta_e_itp_math.h`](../../core/src/feature/delta_e_itp_math.h),
  mirroring the `ssimulacra2_math.h` deterministic-math-helper pattern. A
  committed deterministic PQ LUT for byte-exact cross-host reproducibility
  is deferred to the SIMD/snapshot follow-up.

## Test coverage

`core/test/test_delta_e_itp.c`:

1. **Blue-patch ITP oracle** — the BT.2124-0 Annex 4 normative worked
   example (58 % PQ BT.709-blue patch, 10-bit full-range RGB `[296, 201,
   582]`) converts to the full-precision ITP triple `[0.355721, 0.134647,
   -0.161395]`, asserted at places=4.
2. **Identity** — a pixel compared with itself yields exactly `0.0`.
3. **Synthetic pair** — a fully-specified linear-RGB pair yields the
   reference ΔE-ITP `8.037360` (places=4).
4. **Documentation cross-check** — the standard's pre-rounded ITP triples
   reproduce its printed pooled `2.363` (places=3). (This is a documentation
   cross-check only; the standard's `2.363` is computed from rounded
   intermediates and is *not* used as the extractor oracle.)
5. **PQ transfer round-trip** — `EOTF(EOTF⁻¹(L)) == L`; the blue-patch blue
   channel decodes to `~181.318` cd/m².
6. **End-to-end** — the registered extractor loads, scores an identical
   frame as `0.0`, and a distorted frame as a finite positive value.
7. **Scope guard** — `transfer=hlg` is rejected at init with `-EINVAL`.
