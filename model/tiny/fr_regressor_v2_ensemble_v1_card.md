# `fr_regressor_v2_ensemble_v1` — model card (ensemble)

> Full operator-facing doc: [`docs/ai/models/fr_regressor_v2_probabilistic.md`](../../docs/ai/models/fr_regressor_v2_probabilistic.md)

## Identity

| Field | Value |
| --- | --- |
| Ensemble id | `fr_regressor_v2_ensemble_v1` |
| Members | `fr_regressor_v2_ensemble_v1_seed{0..4}.onnx` (5 members) |
| Manifests | `fr_regressor_v2_ensemble_v1.json` + per-seed `.json` sidecars |
| Architecture | Same as `fr_regressor_v2` but 5× independently seeded full-corpus fits |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production ensemble post-PROMOTE gate (ADR-0303) |

## Training data + provenance

vmaf-tune Phase A JSONL corpus (full corpus, not held-out).
Post-PROMOTE-gate: all 5 seeds are full-corpus fits once the PROMOTE gate
passed. See `fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json` for the
gate-flip record.

## Eval metrics

See `docs/ai/models/fr_regressor_v2_probabilistic.md` for the per-seed
PLCC / SROCC / RMSE and the PROMOTE gate details (ADR-0303).

## Operating point

Run all 5 member ONNXs; aggregate predictions (mean or std for uncertainty).
- **Inputs**: `features` `[N, 6]` + `codec` `[N, 8]` (ENCODER_VOCAB v2 one-hot)
- **Output**: `score` `[N]` per member; ensemble mean / std across members

## Known limits

- ENCODER_VOCAB v2 codec vocabulary (superseded by v3 in `fr_regressor_v3`).
- Ensemble inference requires running 5 separate ONNX sessions.

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0303 for the production-flip protocol.
`registry.json` entries `fr_regressor_v2_ensemble_v1_seed{0..4}`.

## See also

- [`docs/ai/models/fr_regressor_v2_probabilistic.md`](../../docs/ai/models/fr_regressor_v2_probabilistic.md) — full doc
- [`registry.json`](registry.json)
