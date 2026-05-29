# `vmaf_rb_v0.6.2` — model card (Netflix upstream bootstrap ensemble)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/tree/master/model/vmaf_rb_v0.6.2>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_rb_v0.6.2` |
| Files | `vmaf_rb_v0.6.2.json` + `.pkl` + `.pkl.0001..0019` (19 ensemble shards) |
| Architecture | Bootstrap-aggregated Nu-SVR ensemble (19 members), integer-normalised features |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production (older ensemble variant, v0.6.2) |

## Training data + provenance

Netflix proprietary DMOS corpus (1080p SDR). 19-model bootstrap ensemble;
`clip_0to1` normalisation.

## Hyperparameters

19-model bootstrap ensemble (Nu-SVR, RBF, C=4.0, γ=0.04, ν=0.9).
`clip_0to1` normalisation. Score transform: polynomial clamp.

## Eval metrics

Netflix internal evaluation. Ensemble bootstrapping provides model uncertainty
estimates alongside point VMAF scores. Refer to Netflix engineering blog for
qualitative guidance.

## Operating point

- **Backend**: CPU (libsvm ensemble)
- **Resolution**: 1080p SDR and below
- **Bit depth**: 8/10 bpc
- **Input**: 6 canonical features
- **Output**: VMAF score `[0, 100]` + per-member bootstrap distribution

## Known limits

- 19-member ensemble; slower than `vmaf_v0.6.1` (single model).
- v0.6.2 ensemble; `vmaf_rb_v0.6.3` is the newer variant.
- Bootstrap uncertainty interval requires running all 19 members.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_rb_v0.6.3/vmaf_rb_v0.6.3_card.md` — newer bootstrap variant
- `vmaf_v0.6.1_card.md` — single-model default
