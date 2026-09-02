<!-- markdownlint-disable MD060 -->
# ADR-0321: `fr_regressor_v2_ensemble_v1` — full production flip (real ONNX + sidecars)

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `tinyai`, `models`, `registry`, `prod-flip`

## Context

[ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) defined the
production-flip workflow for the deep-ensemble probabilistic head over
`fr_regressor_v2`. [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
hardened the workflow against rebase-time foot-guns: PROMOTE.json is
emitted by the LOSO validator, but the registry flip itself happens
in a dedicated follow-up PR. [ADR-0319](0319-ensemble-loso-trainer-real-impl.md)
landed the real LOSO trainer + corpus loader.

The first flip attempt (PR #423) tried a pure metadata edit on
`model/tiny/registry.json` — only flipping `smoke: true → false` on
the five seed rows. It tripped `core/test/dnn/test_registry.sh`,
which requires a sidecar JSON next to every non-smoke ONNX. Only the
shared ensemble manifest `fr_regressor_v2_ensemble_v1.json` existed;
no per-seed sidecars. PR #423 was closed for redo.

A second concern surfaced at the same time: the 3025-byte ONNX seed
files committed in ADR-0303's scaffold PR were synthetic-corpus
weights (1 epoch each), not the LOSO-validated production weights
the gate had cleared. The registry flip without a re-export would
ship stale weights that did not correspond to PROMOTE.json's
`mean_plcc=0.9973`, `spread=0.00095`, per-seed `>= 0.9968` numbers.

This ADR defines the **proper** production flip: re-train each seed
on the FULL Phase A canonical-6 corpus (5,640 rows over 9 sources +
h264_nvenc), export real ONNX weights, generate per-seed sidecars
with full provenance, update the registry sha256s, and only then
flip `smoke: true → false`.

## Decision

We will produce the production checkpoints via a new driver
`ai/scripts/export_ensemble_v2_seeds.py` that:

1. Reuses `train_fr_regressor_v2_ensemble_loso._load_corpus` so the
   codec block layout (12-slot ENCODER_VOCAB v2 one-hot +
   `preset_norm` + `crf_norm`, total 14 cols) is identical to what
   the LOSO gate validated.
2. Fits one `FRRegressor(in_features=6, hidden=64, depth=2,
   dropout=0.1, num_codecs=14)` per seed on the **full** corpus (no
   held-out fold) — the LOSO PLCC was the gate for whether to ship
   at all; the production checkpoint should see every available row.
3. Exports each seed as
   `model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.onnx` (opset 17,
   two-input contract: `features:[N,6]` + `codec_onehot:[N,14]` →
   `score:[N]`).
4. Writes a per-seed sidecar
   `model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.json` mirroring
   the canonical `fr_regressor_v2.json` shape (encoder vocab,
   codec_block_layout, feature_mean/std, training_recipe) plus
   ensemble-specific fields (`seed`, `loso_mean_plcc`,
   `gate.this_seed_loso_plcc`, `corpus.sha256`,
   `parent_adrs: [ADR-0303, ADR-0309, ADR-0319, ADR-0321]`).
5. Patches the five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
   `model/tiny/registry.json` with the new sha256 and `smoke: false`.

The shared manifest `fr_regressor_v2_ensemble_v1.json` is **not
modified** by this ADR — it tracks the ensemble-mean entry point
and is regenerated only via the trainer. The sidecars are
fresh artefacts.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Full flip with real weights + sidecars (chosen)** | Sidecars satisfy `test_registry.sh`; ONNX bytes match the gate-validated training recipe; sidecars carry per-seed provenance for future audits. | Requires ~2 min of full-corpus training per seed at PR-time; introduces a new driver script. | n/a — this is the chosen option. |
| Metadata-only flip (PR #423 approach) | One-line diff per row; no training compute. | Fails `test_registry.sh` (no sidecars); ONNX bytes are scaffold-era synthetic weights, not the LOSO-validated artefacts PROMOTE.json describes. | Rejected: PR #423 was closed precisely because of this. |
| Wait for the BVI-DVC corpus | Bigger, multi-codec corpus would yield stronger production weights. | BVI-DVC ingestion is not started; PROMOTE.json's gate already passed on the Phase A corpus by a wide margin (mean PLCC 0.997, threshold 0.95); blocking the flip on BVI-DVC indefinitely defeats the purpose of having the gate. | Rejected: the gate is what governs ship/no-ship, not corpus aspiration. |
| Skip the per-seed sidecars; loosen `test_registry.sh` | Smaller diff. | Removes the per-non-smoke-ONNX sidecar contract that protects every other shipped tiny model. | Rejected: the test is the contract; weakening it is forbidden by the [no-test-weakening rule](../../CLAUDE.md). |

## Consequences

- **Positive**: The five ensemble seeds now ship real LOSO-gated
  weights with full provenance. Inference-time consumers can now
  load these models in non-smoke mode and trust the per-seed score
  semantics. The sidecar shape is identical to `fr_regressor_v2.json`
  so downstream loaders that already understand v2 sidecars Just
  Work.
- **Positive**: The training recipe (full-corpus fit, fold-local
  scaler upstream of LOSO, fold-global scaler for the production
  fit) is now baked into each sidecar; future audits can reproduce
  exactly.
- **Negative**: ~10 min of training compute is added to the PR. The
  driver script must stay numerically deterministic across `torch`
  / `numpy` versions — already covered by `_set_seed_all`.
- **Neutral / follow-ups**: Future re-flips (corpus refresh, recipe
  change) **must** re-run `export_ensemble_v2_seeds.py` so ONNX
  bytes + sidecars regenerate together; flipping rows by hand is now
  forbidden by the AGENTS.md invariant added in this PR.

## References

- Parent ADRs: [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md),
  [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md),
  [ADR-0319](0319-ensemble-loso-trainer-real-impl.md).
- Closed PR: #423 (the metadata-only attempt; closed for redo).
- Gate verdict: `runs/ensemble_v2_real/PROMOTE.json` (verdict=PROMOTE,
  mean_plcc=0.9973, spread=0.00095, per-seed >= 0.9968).
- Source: `req` — operator request to do "the proper production flip:
  train + export real ONNX weights per seed, generate per-seed
  sidecars, update registry sha256s, flip smoke=false."
