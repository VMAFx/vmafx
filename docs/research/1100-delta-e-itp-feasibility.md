<!-- markdownlint-disable MD060 -->
# Research-1100: ΔE-ITP (Delta E ITP) — HDR/WCG colour-difference metric feasibility

- **Status**: Closed (ADR-1110 Accepted)
- **Workstream**: full-reference metric inventory; HDR/WCG colour fidelity
- **Last updated**: 2026-06-14
- **Related ADR**: [ADR-1110](../adr/1110-delta-e-itp-metric.md)
- **Source dossier**: `.workingdir2/rc/metrics/deltae-itp.md` (gitignored
  planning artifact; this digest is the tracked summary of its verified
  findings).

## Question

The fork has SDR colour difference (`ciede`, ΔE2000, BT.709-hardcoded) and a
perceptual structural metric (`ssimulacra2`), but **no HDR/WCG
colour-difference metric**. Should the fork add ΔE-ITP (ITU-R BT.2124-0), and
if so, what scope is safe for the first release given that `VmafPicture`
carries no transfer-function metadata?

## Metric summary

ΔE-ITP (ITU-R BT.2124-0, "Objective metric for the assessment of the potential
visibility of colour differences in television") measures the perceptual
visibility of colour differences in HDR (PQ) and HLG video. It is a pure
analytic per-pixel metric (no trained model). The pipeline (Annex 1):

```text
YUV -> R'G'B' -> linear RGB (PQ EOTF) -> LMS (BT.2100 /4096 matrix)
    -> L'M'S' (PQ EOTF^-1) -> ICtCp (BT.2100 /4096 matrix)
    -> ITP (I, 0.5*Ct, Cp)
ΔE_ITP = 720 * sqrt((ΔI)^2 + (ΔT)^2 + (ΔP)^2)
```

≈1.0 corresponds to one just-noticeable colour difference. It maps directly
onto the `VmafFeatureExtractor` interface and mirrors `ciede.c`'s structure
(per-pixel kernel, chroma upsampling, 8/16-bit reads, double-precision frame
sum, mean pooling).

## Constants — adversarial verification

Every constant was confirmed against the **normative ITU-R BT.2124-0 PDF**
(read directly) plus at least one independent source. Triple-confirmed:

- **RGB→LMS** `[1688, 2146, 262 / 683, 2951, 462 / 99, 309, 3688] / 4096` —
  BT.2124-0 Annex 1 + colour-science `ictcp.py` + Wikipedia ICtCp. The
  truncated handwiki row-3 `[993, 668, 3]` is **confirmed wrong** (canonical
  ends 3688); the dzone float `[0.3592, 0.6976, -0.0358]` matrix is a
  **different** (Dolby/XYZ-based) LMS matrix — both traps avoided.
- **PQ ST-2084** `m1 = 2610/16384`, `m2 = 2523/4096·128`, `c1 = 3424/4096`,
  `c2 = 2413/4096·32`, `c3 = 2392/4096·32` — BT.2124-0 + colour-science
  `st_2084.py` + SMPTE ST 2084.
- **L'M'S'→ICtCp** `[2048, 2048, 0 / 6610, -13613, 7003 / 17933, -17390,
  -543] / 4096` — BT.2124-0 + colour-science + Wikipedia.
- **ITP scaling** `I, T = 0.5·Ct, P = Cp` and **ΔE = 720·√Σ** — BT.2124-0
  Annex 1 Steps 4–5 + colour-science `delta_E_ITP`.

**Single-sourced (deferred):** the HLG (Annex 3) OOTF/OETF⁻¹ constants and the
HLG-relative scalings 1.823698 / 1.887755, and the BT.1886/SDR Conversion 5
path (Lw=100, E'^2.4 EOTF + BT.709→BT.2100 primaries). These come only from
BT.2124-0 itself and were not independently cross-confirmed against BT.2100-2.

## Test vector (BT.2124-0 Annex 4 normative worked example)

58 % PQ BT.709-blue patch, 10-bit full-range PQ RGB `[296, 201, 582]`:

| Step | Value | Standard (4-dp rounded) |
|---|---|---|
| E' (÷1023) | [0.2893, 0.1965, 0.5689] | [0.2893, 0.1964, 0.5689] |
| linear RGB (PQ EOTF) | [8.758, 2.294, 181.318] cd/m² | [8.753, 2.291, 181.3] |
| ITP (full precision) | **[0.355721, 0.134647, -0.161395]** | [0.3554, 0.1346, -0.1613] |

The Python pipeline reproduces the standard's printed pooled ΔE = 2.363 from
the **pre-rounded** ITP triples. A full-precision extractor fed the same pair
yields ~2.282, not 2.363. **Conclusion: assert the full-precision ITP triple
at places=4 as the parity gate; use 2.363 only as a documentation
cross-check.** (Implemented exactly this way in `test_delta_e_itp.c`.)

## Decision (verifier's required fixes applied)

- Ship the metric (closes the HDR/WCG colour-fidelity gap). Adversarial
  verdict: GO-WITH-FIXES.
- **PQ-only RC scope** — every shipped constant stays double/triple-confirmed.
  HLG and BT.1886/SDR transfers deferred until independently validated.
- **places=4 ITP-triple oracle**, not pooled==2.363, as the parity assertion.
- CPU scalar only; SIMD/GPU twins, deterministic PQ LUT deferred (same staging
  `ciede` / `ssimulacra2` followed).

See [ADR-1110](../adr/1110-delta-e-itp-metric.md) for the decision record.

## References

- ITU-R BT.2124-0 (01/2019) — Annexes 1–4.
- ITU-R BT.2100; SMPTE ST 2084:2014.
- colour-science/colour (BSD-3-Clause): `ictcp.py`, `st_2084.py`,
  `delta_e.py` — used for cross-validation only, not ported into the tree.
- Wikipedia "ICtCp" — independent matrix confirmation.
