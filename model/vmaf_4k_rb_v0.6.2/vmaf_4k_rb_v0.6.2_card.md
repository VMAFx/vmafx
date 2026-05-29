# `vmaf_4k_rb_v0.6.2` — model card (Netflix upstream 4K bootstrap ensemble)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/tree/master/model/vmaf_4k_rb_v0.6.2>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_4k_rb_v0.6.2` |
| Files | `vmaf_4k_rb_v0.6.2.json` + `.pkl` + `.pkl.0001..0019` (19 ensemble shards) |
| Architecture | Bootstrap-aggregated Nu-SVR ensemble (19 members), 4K-calibrated |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; 4K bootstrap ensemble for uncertainty-aware 4K scoring |

## Training data + provenance

Netflix proprietary 4K DMOS corpus. Bootstrap ensemble variant of
`vmaf_4k_v0.6.1` for 4K content at typical 4K viewing distances.

## Hyperparameters

19-model bootstrap ensemble (Nu-SVR, RBF, C=4.0, γ=0.04, ν=0.9).
4K-specific feature calibration. Score transform: polynomial clamp.

## Eval metrics

Netflix internal 4K DMOS evaluation. Bootstrap distribution provides
confidence intervals on VMAF 4K point estimates.

## Operating point

- **Backend**: CPU (libsvm 19-model ensemble)
- **Resolution**: 4K (3840×2160) — 4K viewing distance calibrated
- **Bit depth**: 8/10 bpc
- **Input**: 6 canonical features
- **Output**: VMAF point score `[0, 100]` + bootstrap distribution

## Known limits

- 4K viewing-distance calibration. Using this on 1080p content will
  produce inflated scores.
- 19-member ensemble; ~19× inference cost vs single-model `vmaf_4k_v0.6.1`.
- SDR only.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_4k_v0.6.1_card.md` — single-model 4K default (faster)
- `vmaf_rb_v0.6.3/vmaf_rb_v0.6.3_card.md` — 1080p bootstrap variant
