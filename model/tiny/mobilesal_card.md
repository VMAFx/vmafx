# `mobilesal_placeholder_v0` — model card (superseded placeholder)

> **Status**: Superseded smoke placeholder. Production saliency weights are in
> `saliency_student_v1` and `saliency_student_v2`. See
> [`docs/ai/models/mobilesal.md`](../../docs/ai/models/mobilesal.md).

## Identity

| Field | Value |
| --- | --- |
| Model id | `mobilesal_placeholder_v0` |
| File | `model/tiny/mobilesal.onnx` |
| Architecture | Synthetic Conv + Sigmoid matching MobileSal I/O contract |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Smoke placeholder — retained for historical ADR references |

## Training data + provenance

Synthetic weights only. The real MobileSal upstream weights were deferred per
ADR-0257 due to CC BY-NC-SA 4.0 license incompatibility and Google-Drive-gated
distribution. The fork-trained `saliency_student_v1` (ADR-0286) replaces this
placeholder for production use.

## Eval metrics

Not applicable — synthetic smoke placeholder; emits plausible per-pixel
saliency in `[0, 1]` but is not trained on any corpus.

## Operating point

- **Input**: `input` — float32 NCHW `[1, 3, H, W]` ImageNet-normalised RGB
- **Output**: `saliency_map` — float32 `[1, 1, H, W]` per-pixel saliency `[0, 1]`

## Known limits

- Not a trained model. Do not use for production saliency estimation.
- Use `saliency_student_v2` (production default since 2026-05-15, ADR-0444).

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0218, ADR-0257, `registry.json` entry
`mobilesal_placeholder_v0`.

## See also

- [`saliency_student_v2_card.md`](saliency_student_v2_card.md) — production saliency
- [`docs/ai/models/mobilesal.md`](../../docs/ai/models/mobilesal.md)
