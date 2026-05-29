# `transnet_v2` — model card (shot-boundary detector)

> Full operator-facing doc: [`docs/ai/models/transnet_v2.md`](../../docs/ai/models/transnet_v2.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `transnet_v2` |
| File | `model/tiny/transnet_v2.onnx` |
| Sidecar | `model/tiny/transnet_v2.json` |
| Architecture | TransNet V2 (Soucek & Lokoc 2020) — LSTM-based shot-boundary detector |
| ONNX opset | 17 |
| License | MIT (upstream soCzech/TransNetV2) |
| Status | Production; upstream weights wrapped for fork I/O contract |

## Training data + provenance

Upstream pretrained weights from Soucek & Lokoc (2020), "TransNet V2:
An effective deep network architecture for fast shot transition detection."
Trained on ClipShots + a private dataset. Upstream pinned at
`github.com/soCzech/TransNetV2@77498b8e`. MIT license.

Wrapper: NTCHW→NTHWC transpose; ColorHistograms `ScatterND`-rewritten for
opset-17 compatibility (ADR-0223). Exported via
`ai/scripts/export_transnet_v2.py`.

## Hyperparameters

Architecture fixed by upstream (LSTM + spatial pyramid pooling on 27×48 RGB
thumbnails). Fork export only adjusts the I/O contract.

## Eval metrics

Refer to the upstream TransNet V2 paper for F1 on ClipShots/RAI. Fork-local
evaluation not yet published in this fork.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Input**: `input` — float32 NCHW `[1, 100, 3, 27, 48]` (100-frame window, RGB thumbnails)
- **Output**: `single_frame_pred` — float32 `[1, 100]` per-frame shot-boundary logit
- **Resolution**: fixed 27×48 thumbnail input (caller must resize upstream)
- **Bit depth**: RGB float

## Known limits

- Upstream weights trained on (mostly) 2D video; may underperform on
  vertical-crop / portrait-orientation content.
- 100-frame sliding window — sharp cuts at window boundaries may be missed.
- `ColorHistograms` branch rewritten for opset-17; numerical parity with
  upstream TF checkpoint is within tolerance, not bit-exact.

## License + lineage

MIT (upstream TransNet V2 weights). Fork wrapper: BSD-3-Clause-Plus-Patent.
See ADR-0223, `registry.json` entry `transnet_v2`.

## See also

- [`docs/ai/models/transnet_v2.md`](../../docs/ai/models/transnet_v2.md) — full doc
- [`registry.json`](registry.json)
