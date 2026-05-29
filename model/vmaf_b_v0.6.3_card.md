# `vmaf_b_v0.6.3` — model card (Netflix upstream phone/broadcast model)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/blob/master/model/vmaf_b_v0.6.3.json>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_b_v0.6.3` |
| File | `model/vmaf_b_v0.6.3.json` |
| Architecture | Ensemble Nu-SVR (21 models), RBF kernel, with bootstrap aggregation |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; phone-screen / broadcast variant |

## Training data + provenance

Netflix proprietary DMOS corpus tuned for phone-screen viewing conditions
(small display, viewing distance ~30–40 cm). v0.6.3 is the bootstrap-
aggregated ensemble variant of the base model.

## Hyperparameters

21-model bootstrap ensemble (Nu-SVR, RBF, C=4.0, γ=0.04, ν=0.9).
`clip_0to1` normalisation with ensemble averaging.

## Eval metrics

Netflix internal evaluation. Refer to Netflix engineering blog for
qualitative guidance on phone-screen use cases.

## Operating point

- **Backend**: CPU (libsvm ensemble)
- **Resolution**: optimised for phone-screen content (< 720p effective)
- **Bit depth**: 8/10 bpc
- **Input**: 6 canonical features
- **Output**: VMAF score `[0, 100]`

## Known limits

- 21-model ensemble is slower than the single-model `vmaf_v0.6.1`.
- Calibrated for phone viewing distance; use `vmaf_v0.6.1` for
  TV/monitor scoring.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_v0.6.1_card.md` — TV / monitor default
- `vmaf_float_b_v0.6.3_card.md` — float-precision broadcast variant
- `vmaf_rb_v0.6.3/vmaf_rb_v0.6.3_card.md` — integer bootstrap variant
