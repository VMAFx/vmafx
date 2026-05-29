# `vmaf_float_v0.6.1` — model card (Netflix upstream float-precision model)

> **Lineage**: Netflix/vmaf upstream. Authoritative source:
> <https://github.com/Netflix/vmaf/blob/master/model/vmaf_float_v0.6.1.json>.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_float_v0.6.1` |
| Files | `model/vmaf_float_v0.6.1.json` + `vmaf_float_v0.6.1.pkl` |
| Architecture | Nu-SVR (libsvm), float-precision feature extraction |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; float-precision variant of `vmaf_v0.6.1` |

## Training data + provenance

Netflix proprietary DMOS training corpus. Same training data as
`vmaf_v0.6.1`; float model uses floating-point feature extraction
instead of the fixed-point pipeline.

## Hyperparameters

Same Nu-SVR parameters as `vmaf_v0.6.1`. Float-precision feature
extraction enables `--vmaf_float_option` CLI path.

## Eval metrics

Same DMOS correlation as `vmaf_v0.6.1` — float variant is numerically
equivalent on the same content. Minor FP rounding differences vs
integer-normalised variant.

## Operating point

- **Backend**: CPU (libsvm); float feature extraction
- **Resolution**: 1080p and below SDR
- **Bit depth**: 8/10 bpc (float features preserve precision)
- **Output**: VMAF score `[0, 100]`

## Known limits

Same as `vmaf_v0.6.1`. Float variant is slower than integer path.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). Verbatim from Netflix/vmaf upstream.

## See also

- `vmaf_v0.6.1_card.md` — integer-normalised default
- `vmaf_float_v0.6.1neg.json` — negative-oriented float variant
