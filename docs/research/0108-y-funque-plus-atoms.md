<!-- markdownlint-disable MD060 -->
# Research-0108: Y-FUNQUE+ wavelet-domain atom features — feasibility + constants

- **Status**: Closed (ADR-1114 Accepted)
- **Workstream**: full-reference metric inventory; FUNQUE-family wavelet metrics
- **Last updated**: 2026-06-14
- **Related ADR**: [ADR-1114](../adr/1114-y-funque-plus-atoms.md)
- **Source dossier**: `.workingdir2/rc/metrics/y-funque-plus.md` (gitignored
  planning artifact; this digest is the tracked summary of its verified
  findings plus an independent re-derivation done during implementation).

## Question

The fork has SSIM/MS-SSIM, ADM/DLM (inside VMAF), VIF, CAMBI, SSIMULACRA2, and
(newly) NIQE / ΔE-ITP — but **no FUNQUE-family wavelet-domain fusion metric**.
Should the fork add Y-FUNQUE+ (the single-channel luma member of the FUNQUE+
suite, Venkataramanan et al., arXiv:2304.03412), and if so, what scope is safe
for the first release given that the fused MOS score requires a trained
regressor that upstream does not ship?

## Metric summary

Y-FUNQUE+ (`Y_FUNQUE_Plus_fex`) per-frame pipeline, luma plane only:

```text
Y -> /((1<<bpc)-1) -> 2x bicubic downscale (OpenCV INTER_CUBIC)
  -> crop to a multiple of 2^levels
  -> 2-level Haar DWT (pywt 'periodization')
  -> Nadenau Y-channel CSF weighting of DETAIL subbands only
  -> { MS-SSIM (cov pooling), DLM (detail-loss), MAD-Ref (temporal) }  [atoms]
  -> ScaledSVR (MinMaxScaler[-1,1] + RBF SVR)  [fused MOS score — DEFERRED]
```

The three atoms are pure analytic functions of the wavelet pyramids and are
fully reproducible in pure double-precision C. The fused score is **not** —
see "Model assets" below.

## Constants — adversarial verification

Every load-bearing constant was confirmed in the dossier against the official
code (`github.com/abhinaukumar/funque_plus`) AND at least one independent
source, then **re-derived independently during implementation** with a faithful
`pywt` + OpenCV `cv2.resize(INTER_CUBIC)` Python reference. Confirmed:

- **Nadenau Y-channel CSF**: analytic form `(1-a)·exp(b·f^c)+a` with
  `a = 1/256`, `b = -5.4715e-3`, `c = 1.91`,
  `level_frequency = (π·1080·3/180)/(1<<(level+1))` (factor =
  56.548667764616276), orientation factors `[1/0.85, 1, 1, 1/0.70]` for
  `[A,H,V,D]`. Regenerates the official lookup table to 8 dp — verified live:
  `nadenau_weight(1, H) = 0.42474743173…` (table 0.42474743),
  `nadenau_weight(0, D) = 0.005562570…` (table 0.00556257). The approx subbands
  are **never** CSF-weighted.
- **MS-SSIM exponents** `[0.0448, 0.2856, 0.3001, 0.2363, 0.1333]` (first 2
  used) — code + paper + Wang/Simoncelli/Bovik 2003.
- **SSIM stabilisers** `C1 = 1e-4`, `C2 = 9e-4` (K1=0.01, K2=0.03, max_val=1).
  Cov pooling = `std(map)/mean(map)`; population std (ddof=0).
- **DLM**: border crop `int(0.2·dim)`, cube-root energy pooling, `eps = 1e-4`;
  decouple `eps = 1e-30`, psi-angle mask `< 1°`, `k = clip(dis/(ref+eps),0,1)`;
  contrast mask `/30`, 3×3 box.
- **Haar butterfly** (pywt `'haar'`, verified directly against `pywt.dwt2`):
  `cA=(a+b+c+d)/2`, `cH=(a+b-c-d)/2`, `cV=(a-b+c-d)/2`, `cD=(a-b-c+d)/2`.

### Verifier fixes folded into the implementation

1. **DLM num/den abs-asymmetry** (`pyr_features.py:54/61`): the numerator pools
   `rest^3` **without** abs; the denominator pools the ref detail **with** abs.
   Both reproduced exactly.
2. **OpenCV INTER_CUBIC** is the **Keys cubic `a = -0.75`** (not textbook
   Catmull-Rom `a = -0.5`); the dossier's "Catmull-Rom" label was loose but the
   operative constant (`-0.75`) is correct. Source coord `2i + 0.5`,
   `BORDER_REPLICATE` edges.
3. **Haar H/V**: the dossier prose listed `cH`/`cV` swapped. A direct
   `pywt.dwt2([[a,b],[c,d]], 'haar')` check shows the implemented form is
   correct and the dossier text was wrong — recorded so a future maintainer
   does not "fix" the code to match the stale prose.
4. **SVR splits**: dossier "5000 splits" corrected to the upstream default
   `100`; irrelevant to the atoms-only port (no SVR shipped).

## Test vector (independently re-derived, places=4)

Identical-input analytic oracles (exact): `ms_ssim = 0`, `dlm = 1`, `mad = 0`
(holds at 8×8, odd 65×33, and the 100×100 crop path). Non-trivial Python-
reference oracles (re-derived live, not taken from the dossier):

| Case | ms_ssim | dlm | mad |
|---|---|---|---|
| 64×64 deterministic, frame 0 | 0.07330722 | 0.99725645 | 0 |
| 64×64 sequence, frame 1 | 1.26043635 | 0.51557907 | 0.11999871 |

The C extractor matches the Python reference to < 1e-10 on these patterns; the
committed gate is the fork's places=4 atom tolerance (5e-5) for cross-host libm
headroom (OpenCV INTER_CUBIC edge handling + pywt periodization wrap are the
dominant deviation sources, hence `-ffp-contract=off` and double precision).

## Model assets — why the fused score is deferred

The upstream `funque_plus` repo ships **no frozen Y-FUNQUE+ regressor**: it
trains a `ScaledSVR` (`MinMaxScaler[-1,1]` + sklearn RBF `SVR`) per dataset at
runtime (default 100 random 80/20 splits) and never commits deployable weights
(tree query: zero `.pkl`/`.json`/`.onnx`/`.npz`). Any fused score would be
fork-originated, with no upstream number to validate against, and would require
a licensed subjective dataset (CC-HDDO / LIVE have their own terms) plus a model
card. **Decision: atoms-only RC; fused SVR deferred** (Deferred row in
`docs/state.md`).

## License

`funque_plus` is **MIT** (Copyright (c) 2023 Abhinau Kumar; LICENSE read
directly this PR), compatible with the fork's BSD-2-Clause-Patent. The metric
math is from published papers and the Nadenau constants are citable research
constants. The C extractor is a **clean-room reimplementation** from the papers
and the dossier, cross-checked against the MIT reference; no Python source was
copied verbatim.

## Decision

GO-WITH-FIXES (atoms-only). Ship `y_funque_plus` with the three atoms; defer the
fused SVR. CPU scalar only; GPU/SIMD twins deferred (would not be bit-exact —
cube-root + exp differ ~1 ulp across hosts/libdevice, consistent with the
fork's GPU-parity posture). See [ADR-1114](../adr/1114-y-funque-plus-atoms.md).

## References

- A. K. Venkataramanan et al., "One Transform To Compute Them All",
  arXiv:2304.03412 (2023).
- A. K. Venkataramanan et al., "FUNQUE: Fusion of Unified Quality Evaluators",
  arXiv:2202.11241 (ICIP 2022).
- M. Nadenau, "Compression of color images with wavelets under consideration of
  the HVS" (CSF constants).
- Z. Wang, E. P. Simoncelli, A. C. Bovik, "Multiscale structural similarity",
  2003 (MS-SSIM exponents).
- `github.com/abhinaukumar/funque_plus` (MIT) — `csf_utils.py`,
  `pyr_features.py`, `dlm_utils.py`, `funque_feature_extractors.py`; used for
  cross-validation only, not ported.
- `pywt` 1.8.0 / OpenCV 4.13.0 — the Python reference that re-derived the oracle
  values.
