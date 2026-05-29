# `vmaf_rb_v0.6.3` — model card (Netflix upstream bootstrap ensemble)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/tree/master/model/vmaf_rb_v0.6.3>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_rb_v0.6.3` |
| Files | `vmaf_rb_v0.6.3.json` + `.pkl` + `.pkl.0001..0020` (20 ensemble shards) |
| Architecture | Bootstrap-aggregated Nu-SVR ensemble (21 models = 1 base + 20 bootstrap), integer-normalised |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; current bootstrap ensemble for uncertainty-aware scoring |

## Training data + provenance

Netflix proprietary DMOS corpus (1080p SDR). 21-model bootstrap ensemble
(v0.6.3 update from v0.6.2). `clip_0to1` normalisation. Same features as
`vmaf_v0.6.1`.

## Hyperparameters

21-model ensemble (Nu-SVR, RBF, C=4.0, γ=0.04, ν=0.9). Score transform:
polynomial clamp `[0.0, 100.0]`.

## Eval metrics

Netflix internal DMOS evaluation. Bootstrap distribution provides 95 %
confidence intervals on VMAF point estimates. Refer to Netflix engineering
blog for bootstrap CI usage guidance.

## Operating point

- **Backend**: CPU (libsvm 21-model ensemble)
- **Resolution**: 1080p SDR and below
- **Bit depth**: 8/10 bpc
- **Input**: 6 canonical features
- **Output**: VMAF point score `[0, 100]` + 20 bootstrap member scores for CI

## Known limits

- 21-member ensemble; ~21× inference cost vs single-model `vmaf_v0.6.1`.
- SDR only — HDR content scores are systematically low (see `vmaf_hdr_model_card.md`).
- Bootstrap CI assumes independent content; correlated frames in a sequence
  inflate the effective N.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_v0.6.1_card.md` — single-model default (faster)
- `vmaf_rb_v0.6.2/vmaf_rb_v0.6.2_card.md` — older bootstrap variant
- `vmaf_float_b_v0.6.3/vmaf_float_b_v0.6.3_card.md` — float-precision broadcast ensemble
