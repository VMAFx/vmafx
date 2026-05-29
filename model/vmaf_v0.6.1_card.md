# `vmaf_v0.6.1` — model card (Netflix upstream production model)

> **Lineage**: Netflix/vmaf upstream. This file documents the fork's
> shipped copy; the authoritative source is
> <https://github.com/Netflix/vmaf/blob/master/model/vmaf_v0.6.1.json>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_v0.6.1` |
| File | `model/vmaf_v0.6.1.json` |
| Architecture | Nu-SVR (libsvm), RBF kernel, C=4.0, γ=0.04, ν=0.9; polynomial score transform |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production default; the fork's golden-gate reference model |

## Training data + provenance

Netflix proprietary DMOS training corpus (not publicly distributed). Features
extracted from professionally produced content at various distortion levels.
Published in: Zhi Li et al., "VMAF: The Journey Continues," Netflix Technology
Blog, 2018; Zhi Li et al., "Toward a Practical Perceptual Video Quality
Metric," 2016. Teacher: DMOS (Differential Mean Opinion Scores).

## Hyperparameters

Nu-SVR: C=4.0, γ=0.04, ν=0.9, RBF kernel. Normalisation: `clip_0to1`.
Score transform: polynomial `p0 + p1*x + p2*x^2` with monotone clamp.
Score clip: `[0.0, 100.0]`. Features: 6 canonical features
(`vif_scale0..3`, `adm2`, `motion`).

## Eval metrics

Published Netflix internal evaluation (DMOS correlation on proprietary test
set). PLCC / SROCC not publicly disclosed per Netflix. External benchmarks:
VMAF v0.6.1 achieves strong correlation with human perception on streamed
video in the 480p–1080p SDR range (see Netflix engineering blog posts).

## Operating point

- **Backend**: CPU (libsvm inference); CUDA/SYCL paths compute features only
- **Resolution**: optimised for 1080p SDR; accurate range 360p–1080p
- **Bit depth**: 8 bpc YUV (primary); 10 bpc with rescaling
- **Input**: 6 canonical features — `vif_scale0..3`, `adm2`, `motion`
- **Output**: VMAF score `[0, 100]`; scores > 100 are clipped

## Known limits

- Calibrated on professionally produced 1080p and below SDR content; scores
  on 4K (use `vmaf_4k_v0.6.1.json`) or HDR (no HDR model shipped; see
  `vmaf_hdr_model_card.md`) may be less accurate.
- May underestimate quality on content types underrepresented in the training
  set (animation, screen content, low-light).

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). This model file is verbatim from
Netflix/vmaf upstream. The fork's Netflix golden-data gate asserts numerical
correctness of this model — its assertions must not be modified.

## See also

- `vmaf_4k_v0.6.1.json` — 4K-tuned variant
- `vmaf_float_v0.6.1.json` — float-precision variant
- `vmaf_b_v0.6.3.json` — phone-optimised variant
- [Netflix VMAF blog](https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12)
