# `vmaf_4k_v0.6.1` — model card (Netflix upstream 4K model)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/blob/master/model/vmaf_4k_v0.6.1.json>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_4k_v0.6.1` |
| File | `model/vmaf_4k_v0.6.1.json` |
| Architecture | Nu-SVR (libsvm), RBF kernel — same architecture as `vmaf_v0.6.1` |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; 4K-tuned variant |

## Training data + provenance

Netflix proprietary 4K DMOS training corpus (not publicly distributed).
Tuned on 4K (3840×2160) content with a viewing-distance-adjusted model.

## Hyperparameters

Same architecture as `vmaf_v0.6.1` (Nu-SVR, RBF). Score transform:
polynomial clamp. Features: 6 canonical + 4K-specific calibration.

## Eval metrics

Netflix internal evaluation on 4K content. Published PLCC/SROCC not
disclosed. Refer to the Netflix VMAF 4K blog post for qualitative guidance.

## Operating point

- **Backend**: CPU (libsvm); GPU computes features
- **Resolution**: 4K (3840×2160) — calibrated viewing distance assumed
- **Bit depth**: 8/10 bpc
- **Input**: 6 canonical features
- **Output**: VMAF score `[0, 100]`

## Known limits

- Calibrated for 4K viewing distance. Using this model on 1080p content
  will produce inflated scores. Use `vmaf_v0.6.1.json` for sub-4K content.
- Same SDR-only limits as the base model; see `vmaf_hdr_model_card.md`.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_v0.6.1_card.md` — 1080p default
- `vmaf_4k_v0.6.1neg.json` — negative-oriented variant
- `vmaf_float_4k_v0.6.1.json` — float-precision 4K variant
