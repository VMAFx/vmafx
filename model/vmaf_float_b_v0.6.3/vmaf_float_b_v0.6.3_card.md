# `vmaf_float_b_v0.6.3` — model card (Netflix upstream float broadcast ensemble)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/tree/master/model/vmaf_float_b_v0.6.3>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_float_b_v0.6.3` |
| Files | `vmaf_float_b_v0.6.3.json` + `.pkl` + `.pkl.0001..0020` (sharded) |
| Architecture | 21-model bootstrap ensemble Nu-SVR, float-precision feature extraction |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; float-precision phone/broadcast ensemble |

## Training data + provenance

Netflix proprietary DMOS corpus, phone-screen viewing conditions. 21-model
bootstrap ensemble with float-precision features. Same corpus as `vmaf_b_v0.6.3`
but with floating-point feature extraction pipeline.

## Hyperparameters

21-model ensemble (Nu-SVR, RBF, C=4.0, γ=0.04, ν=0.9). Float-precision
feature extraction. Score transform: polynomial clamp.

## Eval metrics

Same DMOS correlation as `vmaf_b_v0.6.3`. Float variant is numerically
near-identical; minor FP rounding differences. Netflix internal evaluation only.

## Operating point

- **Backend**: CPU (libsvm ensemble, float feature path)
- **Resolution**: optimised for phone-screen / broadcast (< 720p effective)
- **Bit depth**: 8/10 bpc (float preserves precision)
- **Input**: 6 canonical features
- **Output**: VMAF score `[0, 100]`

## Known limits

- 21-model ensemble; slower than single-model variants.
- Phone/broadcast viewing-distance calibration — not appropriate for 4K TV scoring.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_b_v0.6.3_card.md` — integer-normalised broadcast variant
- `vmaf_rb_v0.6.3/` — integer bootstrap variant with same sharded structure
