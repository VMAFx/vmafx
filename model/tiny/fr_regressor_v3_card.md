# `fr_regressor_v3` — model card

> Full operator-facing doc: [`docs/ai/models/fr_regressor_v3.md`](../../docs/ai/models/fr_regressor_v3.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `fr_regressor_v3` |
| File | `model/tiny/fr_regressor_v3.onnx` |
| Sidecar | `model/tiny/fr_regressor_v3.json` |
| Architecture | Tiny MLP — 6 canonical features + 18-D codec block → 24 inputs |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production checkpoint (codec-aware, ENCODER_VOCAB v3) |

## Training data + provenance

Phase A canonical-6 JSONL corpus (`runs/phase_a/full_grid/per_frame_canonical6.jsonl`,
sha256: `58512e6c...`), 5640 rows. 9-fold LOSO (one source held out per fold).
Teacher label: `vmaf_v0.6.1` per-frame score. ENCODER_VOCAB v3: 16 encoder
one-hots including AMF (h264/hevc/av1) and Apple VideoToolbox.

## Hyperparameters

24 inputs (6 canonical + 18 codec block), hidden 64, 1 output. Feature
standardisation stats and codec vocab in sidecar JSON. Ship gate: LOSO mean
PLCC ≥ 0.95.

## Eval metrics (9-fold LOSO, per-fold)

| Held-out source | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: |
| BigBuckBunny | 0.9973 | 0.9878 | 0.787 |
| BirdsInCage | 0.9988 | 0.9989 | 0.432 |
| CrowdRun | 0.9996 | 0.9972 | 0.677 |
| ElFuente1 | 0.9987 | 0.8805 | 0.822 |
| ElFuente2 | 0.9950 | 0.9984 | 3.288 |
| FoxBird | 0.9945 | 0.9329 | 0.904 |
| OldTownCross | 0.9981 | 0.9951 | 0.810 |
| Seeking | 0.9989 | 0.9877 | 1.013 |
| Tennis | 0.9962 | 0.9436 | 1.061 |
| **Mean** | **0.9975** | **0.9691** | **1.088** |

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Inputs**: `features` `[N, 6]` + `codec_block` `[N, 18]` (ENCODER_VOCAB v3)
- **Output**: `vmaf` `[N]` float32, VMAF scale `[0, 100]`
- **Resolution**: any (feature-based); **Bit depth**: 8/10 bpc

## Known limits

- Codec vocabulary fixed at ENCODER_VOCAB v3 (16 encoders). Unknown encoders
  map to the `encoder_onehot[unknown]` slot — accuracy may degrade.
- Per-source RMSE inflates on high-motion sources (ElFuente2: 3.29 VMAF).

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0323, ADR-0302, ADR-0291, ADR-0235.
`registry.json` entry `fr_regressor_v3`.

## See also

- [`docs/ai/models/fr_regressor_v3.md`](../../docs/ai/models/fr_regressor_v3.md) — full doc
- [`registry.json`](registry.json)
