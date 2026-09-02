<!-- markdownlint-disable MD013 MD060 -->
# ADR-0235: Codec-aware FR regressor (`fr_regressor_v2`)

- **Status**: Accepted
- **Date**: 2026-05-01
- **Deciders**: Lusoris, Claude
- **Tags**: `ai`, `dnn`, `tiny-ai`, `fr-regressor`, `fork-local`

## Context

The fork ships a tiny full-reference (FR) MOS regressor —
`ai/src/vmaf_train/models/fr_regressor.py` (PyTorch) → `model/tiny/fr_regressor_v1.onnx`
— that maps a libvmaf `FULL_FEATURES` vector (adm / vif / motion / psnr /
ssim / cambi / ssimulacra2 / ciede / psnr-hvs) to a single MOS scalar.
The v1 baseline is **codec-blind**: every distorted clip lands on the
same MLP regardless of which encoder produced it.

Distortion signatures from x264, x265, libsvtav1, libvvenc, and
libvpx-vp9 differ systematically — block edges (x264) vs CTU-boundary
blur (x265) vs DCT ringing + restoration filters (AV1) vs large-CTU
deblocking (VVC). The 2026 Bristol VI-Lab review §5.3 (`.workingdir2/preprints202604.0035.v1.pdf`)
calls this directly: conditioning a perceptual regressor on codec id
"reliably lifts cross-codec PLCC/SROCC by 1-3 points on multi-codec
corpora". Bampis 2018 (ST-VMAF) and Zhang 2021 (Bull lab "Enhancing
VMAF") both report similar deltas in their per-codec ablations.

The fork's training corpus is heading toward multi-codec coverage —
KoNViD-1k ships natural distortions, BVI-DVC + the Netflix Public
corpus ship single-codec material, and the next sweep adds
libsvtav1 + libvvenc legs. A codec-blind v1 is increasingly
mis-specified; we either condition the model now or accept a
permanent ceiling on cross-codec PLCC.

## Decision

We will (a) capture an explicit `codec` column in the per-clip
parquet output of every feature-dump script under `ai/scripts/`,
(b) extend `FRRegressor` with an optional `num_codecs` constructor
arg that adds a one-hot codec input concatenated to the feature
vector before the first MLP layer, and (c) ship a closed,
order-stable codec vocabulary in `ai/src/vmaf_train/codec.py`
(`x264`, `x265`, `libsvtav1`, `libvvenc`, `libvpx-vp9`, `unknown`).
The default `num_codecs=0` keeps the v1 single-input contract so
existing checkpoints load unchanged. A `fr_regressor_v2_codec_aware`
checkpoint registers under `model/tiny/registry.json` only after a
side-by-side training run shows a positive (>0.005) PLCC lift on a
held-out multi-codec split — otherwise we document the negative
result and stop.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **One-hot codec concatenated to feature vector** (chosen) | Simple, deterministic, preserves the v1 ONNX op-allowlist (no `Embedding` op needed at runtime), trivial backwards-compat via `num_codecs=0` | Wastes ~6 input dims; won't generalise to unseen codecs | Wins on simplicity + ONNX compatibility |
| Per-codec sub-models with a router | Highest possible quality ceiling per codec | 6× the parameter budget; routing logic doubles the runtime DNN surface; no fallback for "unknown" | Too expensive for a "tiny" model; "unknown" bucket has no sub-model |
| Continuous learned embedding (`nn.Embedding(num_codecs, d_emb)`) | Compact (4–8 dims vs 6), graceful via `unknown` index | Requires an additional ONNX op (`Gather`) on the allowlist; doesn't measurably outperform one-hot at this scale | Adds runtime complexity for no measurable accuracy delta on 6 codecs |
| Skip codec conditioning, train on more data instead | No model surgery | Bristol VI-Lab §5.3 explicitly calls out that no amount of single-corpus data closes the cross-codec gap; the gap is structural | Direct contradiction of the cited literature |

## Consequences

- **Positive**: cross-codec PLCC/SROCC lift expected at 1–3 points
  per the Bristol review; trivial to extend the vocabulary as new
  codecs land (append to `CODEC_VOCAB`, bump `CODEC_VOCAB_VERSION`,
  retrain); `extract_full_features.py` self-describes its corpus
  with `codec="unknown"` rather than silently mislabelling.
- **Negative**: ONNX graph gains a second input — libvmaf's
  `vmaf_dnn_session_run` already supports two-input contracts (LPIPS-Sq
  precedent in ADR-0040 / ADR-0041), but the C-side wiring for
  `fr_regressor` will need the same multi-input pattern in a
  follow-up PR; the current PR is training-side only.
- **Neutral / follow-ups**: training run + PLCC delta measurement
  is **blocked** in the present PR (the agent that proposed the
  change cannot reach `~/.cache/vmaf-tiny-ai/`); follow-up PR
  re-runs the trainer + measures + ships `fr_regressor_v2_codec_aware.onnx`
  if the lift exceeds the 0.005 PLCC bar. Backlog item: T7-CODEC-AWARE.

## References

- Bristol VI-Lab review (Bull, Zhang) — `.workingdir2/preprints202604.0035.v1.pdf`,
  §5.3 "Codec-conditioned quality models".
- Bampis et al. 2018, "Spatiotemporal Feature Integration and Model Fusion for Full-Reference Video Quality Assessment" (ST-VMAF), IEEE TCSVT 28(8).
- Zhang, Bull et al. 2021, "Enhancing VMAF through new feature integration and model combination" — Bull lab follow-up to Bampis 2018, per-codec ablation.
- Prior ADRs: [ADR-0020](0020-tinyai-four-capabilities.md) (C1 capability),
  [ADR-0040](0040-dnn-session-multi-input-api.md) (multi-input session API),
  [ADR-0041](0041-lpips-sq-extractor.md) (two-input precedent),
  [ADR-0042](0042-tinyai-docs-required-per-pr.md) (model-card bar),
  [ADR-0168](0168-tinyai-konvid-baselines.md) (corpus baselines).
- Research digest: [Research-0040](../research/0040-codec-aware-fr-conditioning.md).
- Source: `req` — user task brief 2026-05-01 ("Run a codec-aware
  feature experiment for the FR regressor").

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `ai/src/vmaf_train/codec.py` — present (closed,
  order-stable codec vocabulary).
- `FRRegressor` constructor accepts `num_codecs`; the codec one-hot
  is concatenated to the canonical-6 feature vector before the first
  MLP layer (verified in
  `ai/src/vmaf_train/models/fr_regressor.py`).
- `model/tiny/fr_regressor_v2.{onnx,json,onnx.data}` registered;
  the deep-ensemble companion `fr_regressor_v2_ensemble_v1` ships
  five seeded ONNX members (`_seed{0..4}`) per ADR-0279 (this sweep,
  Accepted).
- ADR-0272 (this sweep, Accepted) is the v2 scaffold that consumes
  the codec column from the vmaf-tune Phase A JSONL corpus.
- Verification command:
  `grep -E "num_codecs" ai/src/vmaf_train/models/fr_regressor.py;
  ls ai/src/vmaf_train/codec.py model/tiny/fr_regressor_v2*`.
