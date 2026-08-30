<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1112: NIQE no-reference CPU feature extractor (fork-pkl parity)

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: metrics, feature-extractor, no-reference, cpu, model, fork-local

## Context

The fork already ships a trained pristine NIQE model
(`model/other_models/niqe_v0.1.pkl`, 36-dim multivariate-Gaussian) and a
Python harness (`compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor`)
that produces NIQE scores, but there was no C feature extractor — so NIQE
could not run through the `vmaf` CLI, the libvmaf C API, or the ffmpeg filter.
NIQE (Mittal, Soundararajan, Bovik, IEEE SPL 20(3), 2013) is a no-reference,
opinion-unaware blind IQA metric: it scores a single picture against the
natural-scene statistics of a pristine population, so it needs neither a
reference frame nor subjective opinion scores.

We need a C extractor that produces the **same** scores the fork's Python
harness already emits, because the pristine `.pkl` was trained against that
exact pipeline. The fork's NIQE pipeline diverges from upstream NIQE
implementations (LIVE MATLAB, scikit-video) in two load-bearing ways:
the AGGD mean parameter `N` carries a trailing `*aggdratio` factor, and the
per-patch MSCN maps are round-tripped through float32 before feature
extraction. Matching upstream NIQE instead would require a different pristine
model with a different feature order — mutually exclusive with fork-pkl
parity.

## Decision

We will add a scalar CPU NIQE extractor (`core/src/feature/niqe.c`, feature
name `niqe`) that replicates the fork harness byte-for-byte: separable 7-tap
Gaussian MSCN (sigma=7/6, C=1, nearest boundary) in float64; a PIL-compatible
Catmull-Rom (a=-0.5) bicubic half-resolution downscale whose output is rounded
to float32 (PIL 'F' mode); per-patch AGGD fits in float32 with the
fork-specific trailing `*aggdratio` in `N`; pooling to a sample mean +
unbiased covariance; and the Mahalanobis distance
`sqrt(X^T pinv((cov_pris + sample_cov)/2) X)` using a symmetric pseudo-inverse
with scipy's default cutoff `rtol = 36 * eps`. The pristine model is the
fork's own `niqe_v0.1.pkl`, embedded as `core/src/feature/niqe_model.h` in the
interleaved per-block feature order the extractor emits. As an NR metric the
extractor scores only the distorted picture and `(void)`-discards the
reference and the 90-degree-rotated inputs (mirroring CAMBI's NR posture).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Fork `niqe_v0.1.pkl` + replicate fork harness (chosen)** | Matches the score the fork already produces; no new model asset; no new license; already trained on the Netflix corpus against this exact pipeline | Diverges from upstream NIQE (the `*aggdratio` quirk + float32 round-trip), so the C scores will not match reference LIVE/skvideo NIQE | — |
| LIVE reference `modelparameters.mat` + MATLAB `computefeature.m` | Matches the canonical upstream NIQE; widely cited | Different feature order, different `N` formula (no `*aggdratio`), different training corpus (125 LIVE pristine images), separate UT-Austin research license to vet; would not match the fork harness | Mutually exclusive with fork-pkl parity; would silently change scores vs the harness the fork already ships |
| float64 throughout (skip the float32 MSCN round-trip + float32 bicubic) | Simpler, marginally more precise | The `.pkl` was trained on float32-quantized features; skipping the round-trip shifts every feature by ~1e-7 and, amplified by the ill-conditioned averaged covariance, the score by up to ~1e-4 on natural content (and far more on adversarial synthetic input) | Breaks places=4 parity on the harness reference; the float32 steps are load-bearing |

## Consequences

- **Positive**: NIQE is now a first-class CPU feature reachable from the CLI
  (`--feature niqe`), the C API, and (once wired) the ffmpeg filter. Scores
  match the fork Python harness at places=4+ on natural content (measured
  ~1.4e-7 on frame 0 of `testdata/ref_576x324_48f.yuv`). No new dependency,
  no new model asset, no new license obligation.
- **Negative**: The fork's NIQE will not match upstream reference NIQE
  implementations (documented limitation, driven by the `*aggdratio` quirk).
  The metric scores raw luma with no transfer-function awareness, so HDR
  (PQ/HLG) input is scored as-is — the natural-scene-statistics assumptions
  break down for non-SDR content. High-bit-depth (>8 bpc) input feeds raw
  luma values; because the MSCN `C=1` stabiliser is not scale-invariant,
  >8-bpc parity to the 8-bit-trained model is not guaranteed (see open
  follow-ups).
- **Neutral / follow-ups**: GPU twins (CUDA/SYCL/HIP) are out of scope for
  this PR; a future twin must keep the 36x36 covariance + pinv in fp64 and is
  not expected to be bit-exact to CPU (per the fork's GPU-parity posture,
  places=4). Open questions for a later ADR: an explicit >8-bpc scaling policy
  and an optional `model_path` option for a user-supplied pristine model.
  Documented in [`docs/state.md`](../state.md) (Deferred).

## References

- Mittal, Soundararajan, Bovik, "Making a Completely Blind Image Quality
  Analyzer," IEEE SPL 20(3):209-212, 2013 —
  <http://live.ece.utexas.edu/research/Quality/niqe_spl.pdf>
- Fork ground truth — `compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor`
  and `compat/python-vmaf/core/niqe_train_test_model.py::_predict`
- Fork asset — `model/other_models/niqe_v0.1.pkl` (36-dim mu + 36x36 cov)
- LIVE NIQE MATLAB release (utlive/niqe) and scikit-video `skvideo/measure/niqe.py`
  (cross-checked for every constant; both omit the `*aggdratio` factor)
- Design dossier — `.workingdir2/rc/metrics/niqe.md` (math + model adversarially confirmed)
- Source: `req` (user direction to finish the NIQE no-reference CPU feature extractor matching the fork's trained model)
