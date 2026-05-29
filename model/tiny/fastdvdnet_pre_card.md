# `fastdvdnet_pre` — model card (temporal pre-filter)

> Full operator-facing doc: [`docs/ai/models/fastdvdnet_pre.md`](../../docs/ai/models/fastdvdnet_pre.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `fastdvdnet_pre` |
| File | `model/tiny/fastdvdnet_pre.onnx` |
| Sidecar | `model/tiny/fastdvdnet_pre.json` |
| Architecture | FastDVDnet (Tassano, Delon & Veit 2020) — 5-frame temporal denoiser |
| ONNX opset | 17 |
| License | MIT (upstream m-tassano/fastdvdnet) |
| Status | Production; upstream weights wrapped for libvmaf 5-frame luma contract |

## Training data + provenance

Upstream pretrained weights from Tassano, Delon & Veit (2020), "FastDVDnet:
Towards Real-Time Deep Video Denoising Without Flow Estimation." Trained on
Davis 2017 + BSD500 with synthetic AWGN. Upstream pinned at
`github.com/m-tassano/fastdvdnet@c8fdf61`. MIT license.

Luma adapter: Y→[Y,Y,Y] tile, σ=25/255 noise map, RGB→Y BT.601 collapse;
preserves the libvmaf 5-frame luma I/O contract (ADR-0215). Exported via
`ai/scripts/export_fastdvdnet_pre.py`.

## Hyperparameters

Architecture fixed by upstream (U-Net-style denoiser, 5 frames). σ fixed at
25/255 in the wrapper.

## Eval metrics

Refer to upstream FastDVDnet paper for PSNR on Davis 2017. Fork-local VMAF
delta from pre-filter not yet published.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Input**: 5 consecutive luma frames `[1, 5, H, W]` float32 `[0, 1]`
- **Output**: restored luma `[1, 1, H, W]` float32
- **Resolution**: any (fully convolutional)
- **Bit depth**: 8 bpc luma (caller converts 10 bpc if needed)

## Known limits

- σ=25/255 fixed — sub-optimal for very low or very high noise levels.
- Luma-only; no chroma filtering.
- Upstream MIT license applies to the weights.

## License + lineage

MIT (upstream FastDVDnet weights). Fork wrapper: BSD-3-Clause-Plus-Patent.
See ADR-0215, `registry.json` entry `fastdvdnet_pre`.

## See also

- [`docs/ai/models/fastdvdnet_pre.md`](../../docs/ai/models/fastdvdnet_pre.md) — full doc
- [`registry.json`](registry.json)
