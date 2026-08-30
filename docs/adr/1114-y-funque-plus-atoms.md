<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1114: Y-FUNQUE+ wavelet-domain atom features (atoms-only, fused SVR deferred)

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: metrics, feature-extractor, full-reference, cpu, wavelet, license, fork-local

## Context

Y-FUNQUE+ (`Y_FUNQUE_Plus_fex`) is the single-channel (luma) member of the
official FUNQUE+ full-reference VQA suite (Venkataramanan et al.,
arXiv:2304.03412 "One Transform To Compute Them All"; FUNQUE,
arXiv:2202.11241). Its per-frame pipeline downscales luma 2x, runs a 2-level
Haar wavelet transform, applies a Nadenau HVS-CSF subband weighting, and
derives three wavelet-domain atom features — MS-SSIM with covariance pooling,
a DLM detail-loss measure, and a MAD-Ref temporal atom — which a trained
ScaledSVR then fuses into a single MOS-predicting score.

The critical gap (confirmed by the verified design dossier
`.workingdir2/rc/metrics/y-funque-plus.md`) is that the upstream `funque_plus`
repository ships **no frozen regressor**: it trains a `ScaledSVR`
(`MinMaxScaler[-1,1]` + sklearn RBF `SVR`) per dataset at runtime (default 100
random 80/20 splits) and never commits deployable weights. Any fused
Y-FUNQUE+ score would therefore be fork-originated, requiring a licensed
subjective dataset and a model card. The three raw atom features, by contrast,
are fully reproducible in pure C from published constants.

A license check was required before porting any reference code. The
`funque_plus` repository (github.com/abhinaukumar/funque_plus) is **MIT
licensed** (Copyright (c) 2023 Abhinau Kumar), which is compatible with this
fork's BSD-2-Clause-Patent. The metric math itself derives from published
papers and the Nadenau CSF research constants are citable, not copyrightable.
The C extractor is a clean-room reimplementation guided by the papers and the
dossier; the MIT-licensed reference was used only as an algorithmic
cross-check and no Python source was copied verbatim.

## Decision

We will ship the three Y-FUNQUE+ wavelet-domain **atom features only**, as a
CPU feature extractor `y_funque_plus` (`core/src/feature/y_funque_plus.c`,
temporal), emitting `y_funque_plus_ms_ssim`, `y_funque_plus_dlm`, and
`y_funque_plus_mad`. We will **not** train or ship a fused SVR score in this
PR; the fused score is a deferred follow-up tracked in
[`docs/state.md`](../state.md). All arithmetic is double-precision. The
pipeline faithfully reproduces: the OpenCV `INTER_CUBIC` 2x pre-downscale
(Keys cubic `a = -0.75`, source coordinate `2i + 0.5`, `BORDER_REPLICATE`
edges — the dominant parity risk, verified bit-exact against `cv2`); the
2-level Haar DWT in pywt `'periodization'` convention; the Nadenau Y-channel
CSF weighting of detail subbands only (analytic form with `a = 1/256`,
`b = -5.4715e-3`, `c = 1.91`, regenerating the official lookup table to 8 dp);
MS-SSIM covariance pooling with exponents `[0.0448, 0.2856]` and
`C1 = 1e-4 / C2 = 9e-4`; and the DLM decouple + contrast-mask + cube-root
pooling. The DLM numerator/denominator abs-asymmetry is replicated exactly:
the numerator pools `rest^3` **without** abs while the denominator pools the
reference detail **with** abs (per `pyr_features.py` lines 54/61). The
extractor is compiled in its own static library with `-ffp-contract=off` (the
same cross-host-determinism rationale as `ssimulacra2`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Atoms-only, fused SVR deferred (chosen)** | RC-feasible pure C; no model asset; no license-on-weights risk; the atoms are fully reproducible from published constants; lets users fuse externally or wait for a fork-trained SVR | No single Y-FUNQUE+ MOS score yet; consumers must combine the three atoms themselves | — |
| Train + freeze a fork SVR now and ship a fused score | A single deployable Y-FUNQUE+ number out of the box | Requires a licensed subjective dataset (CC-HDDO / LIVE have their own usage terms), a model card, and the fused number would be fork-originated with no upstream reference to validate against; expands PR scope and license surface materially | Deferred — the maintainer chose atoms-first for RC; the SVR is a separate, dataset-gated PR |
| Clean-room from papers only (ignore the MIT reference) | Zero dependency on the upstream repo | Unnecessary: the reference is MIT (BSD-2-Clause-Patent-compatible), so it is a legitimate algorithmic cross-check; discarding it would only weaken constant verification | Not needed once the MIT license was confirmed |

## Consequences

- **Positive**: Y-FUNQUE+'s perceptually-weighted wavelet atoms are now a
  first-class CPU feature reachable from the `vmaf` CLI (`--feature
  y_funque_plus`), the libvmaf C API, and (once wired) the ffmpeg filter. The
  C atoms match a faithful Python reference (pywt + OpenCV) to better than
  1e-10 on deterministic input and bit-for-bit on the Netflix 576x324 fixture,
  well inside the fork's places=4 atom gate. No new dependency, no model
  asset, no license obligation beyond MIT attribution (recorded here and in
  the source header).
- **Negative**: There is no fused Y-FUNQUE+ MOS score yet — consumers get
  three atoms, not one number. The metric assumes SDR code-value luma (no
  EOTF); HDR (PQ/HLG) input is scored as-is (documented limitation — a
  Cut-FUNQUE/HDR-FUNQUE PU21 pre-step would be a separate variant). The 2x
  bicubic downscale and pywt periodization are the dominant cross-host parity
  risks; the `-ffp-contract=off` build and double precision keep them inside
  places=4.
- **Neutral / follow-ups**: the fused ScaledSVR score is deferred (a Deferred
  row in `docs/state.md`); it needs a licensed subjective dataset, a frozen
  regressor exported as constants or a model JSON, and a model card. GPU twins
  (CUDA/SYCL/HIP) are out of scope and would not be bit-exact to CPU (the
  fork's GPU-parity posture is places=4); a deterministic `cbrt`/`exp` helper
  analogous to `ssimulacra2_math.h` would be needed for a cross-host snapshot
  gate.

## Supply-chain impact

- **New dependencies**: none. The extractor is pure C against libm; no new
  runtime, build, or test dependency. The MIT-licensed `funque_plus` repo is
  not vendored — it was an algorithmic reference only.
- **CVE surface delta**: none (no new library, listener, or syscall).

## References

- Official code: github.com/abhinaukumar/funque_plus — MIT License,
  Copyright (c) 2023 Abhinau Kumar (verified via WebFetch this PR);
  `feature_extractors/funque_feature_extractors.py` (`YFunquePlusFeatureExtractor`),
  `features/funque_atoms/{csf_utils,pyr_features,dlm_utils}.py`
- A. K. Venkataramanan, C. Stejerean, I. Katsavounidis, A. C. Bovik,
  "One Transform To Compute Them All: Efficient Fusion-Based Full-Reference
  Video Quality Assessment", arXiv:2304.03412 (2023)
- A. K. Venkataramanan et al., "FUNQUE: Fusion of Unified Quality Evaluators",
  ICIP 2022 / arXiv:2202.11241
- M. Nadenau, "Compression of color images with wavelets under consideration
  of the HVS" — analytic CSF constants (a, b, c)
- Z. Wang, E. P. Simoncelli, A. C. Bovik, "Multiscale structural similarity
  for image quality assessment", 2003 — MS-SSIM exponents
- Design dossier — `.workingdir2/rc/metrics/y-funque-plus.md` (math + every
  load-bearing constant adversarially re-derived and confirmed)
- Research digest — [`docs/research/0108-y-funque-plus-atoms.md`](../research/0108-y-funque-plus-atoms.md)
- Source: `req` (maintainer direction to ship Y-FUNQUE+ atoms-only for RC,
  fused SVR deferred; port-vs-clean-room recorded above)
