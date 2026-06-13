# ADR-0272: `fr_regressor_v2` codec-aware scaffold (Phase B prereq)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude
- **Tags**: `ai`, `dnn`, `tiny-ai`, `fr-regressor`, `codec-aware`,
  `vmaf-tune`, `fork-local`

## Context

[ADR-0249](0249-fr-regressor-v1.md) shipped `fr_regressor_v1`, a
codec-blind tiny MLP that maps the canonical-6 libvmaf feature vector
(`adm2`, `vif_scale0..3`, `motion2`) to a VMAF teacher score with
9-fold LOSO mean PLCC = 0.9977 ± 0.0025 on the Netflix Public corpus.
[ADR-0235](0235-codec-aware-fr-regressor.md) decided that the next
generation of the regressor would be codec-aware — the canonical-6
features alone cannot distinguish x264 block edges from libvvenc CTU
deblocking from libsvtav1 in-loop restoration filters, and the
Bristol VI-Lab review §5.3 reports a 1–3 PLCC-point lift from codec
conditioning on multi-codec corpora.

The blocker for ADR-0235's training run was a multi-codec corpus.
[ADR-0237](0237-quality-aware-encode-automation.md) chartered
`vmaf-tune` Phase A — a corpus orchestrator that sweeps a
`(preset, crf)` grid against raw YUV sources for a chosen encoder
and emits one JSONL row per cell. Phase A landed in PR #329; the
schema (`tools/vmaf-tune/src/vmaftune/corpus.py`) carries `encoder`,
`preset`, `crf`, `bitrate_kbps`, `vmaf_score`, etc. v2 is the first
downstream consumer of that schema.

This ADR scopes the **scaffold**: the training script,
synthetic-corpus smoke path, registry entry (gated `smoke: true`),
and ONNX export plumbing. The actual training run on a real Phase A
corpus is a follow-up — see [Research-0054](../research/0058-fr-regressor-v2-feasibility.md)
§"Open question: corpus diversity for production v2".

## Decision

We will ship `ai/scripts/train_fr_regressor_v2.py` as a scaffold-only
trainer. It (a) loads the vmaf-tune Phase A JSONL corpus, (b)
materialises a two-input feature space (`features` = 6 canonical-6
dims, `codec` = 8-D block: 6-way encoder one-hot + preset_norm in
[0, 1] + crf_norm in [0, 1]), (c) trains the existing
`FRRegressor(in_features=6, num_codecs=8)` (the class already supports
codec conditioning per ADR-0235), (d) bakes the StandardScaler over
the canonical-6 dims into the sidecar JSON (codec block stays
unscaled — already in [0, 1]), and (e) exports a two-input ONNX
matching the LPIPS-Sq precedent (ADR-0040 / ADR-0041). The registry
entry registers `fr_regressor_v2` with `smoke: true` until a real
Phase A corpus has been produced and a follow-up PR re-runs training,
clears the LOSO ship gate, and flips the flag.

A `--smoke` mode synthesises 100 fake corpus rows from a deterministic
CRF-vs-VMAF function so the pipeline is end-to-end exercisable in CI
without hours of encode time. The smoke ONNX is the registered
artifact in this PR; it is a load-path probe, not a quality model,
exactly mirroring the existing `smoke_v0.onnx` precedent.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Two-input ONNX (`features` + `codec`), scaffold ships smoke ONNX** (chosen) | Reuses LPIPS-Sq two-input pattern; smoke ONNX validates the export+roundtrip path; clear `smoke: true` flag in registry; production run is a clean follow-up gated by the ship threshold | Registers a placeholder ONNX in-tree (~few KB); reviewers must trust the `smoke` flag | Wins on validating the pipeline today and unblocking the corpus run cleanly |
| Single-input 14-D ONNX | One tensor at runtime, slightly simpler libvmaf wiring | Loses semantic boundary between feature and codec block; can't gate codec input off for codec-blind fallback inference | Two-input matches LPIPS-Sq + future-proofs codec-blind fallback |
| Defer the scaffold; wait for the real corpus first | Avoids landing a placeholder ONNX | Couples Phase B/v2 unblock to a multi-day corpus run; loses CI signal that the pipeline still works end-to-end | The corpus run is hours-to-days; the scaffold gives reviewers something concrete to bikeshed before that compute is spent |
| Continuous learned codec embedding (`nn.Embedding`) | Compact, graceful for unknown codecs | Adds `Gather` to op-allowlist; no measurable accuracy delta at this corpus scale per ADR-0235 §Alternatives | Picked one-hot for the same reasons ADR-0235 did; consistency |
| New `FRRegressorV2` class | Could diverge architecturally from v1 | Doubles the maintenance surface for one extra concat | The existing `FRRegressor` already supports `num_codecs` (added by ADR-0235); reuse |

## Consequences

- **Positive**: vmaf-tune Phase A has a documented downstream
  consumer; the v2 training pipeline is exercised in CI via the smoke
  path so any regression in the FRRegressor / op-allowlist /
  torch-onnx export chain surfaces before the production training
  run; reviewers can iterate on the codec-block layout (preset
  ordinal, CRF normaliser) before any compute is spent.
- **Negative**: a placeholder `fr_regressor_v2.onnx` lives in-tree
  with a `smoke: true` flag — registry consumers must respect the
  flag and skip the model in quality-metric runs; the smoke ONNX
  bytes will be replaced when the production training run lands, so
  the `sha256` in the registry will rotate then; corpus rows in
  Phase A's current schema do **not** carry per-frame canonical-6
  features (only an aggregate `vmaf_score`), so the production run
  needs a Phase A schema follow-up that attaches per-frame features
  to each row, or a feature-extraction step that re-runs libvmaf in
  per-frame mode against the corpus's encode artifacts.
- **Neutral / follow-ups**: production training run is gated on
  (1) a multi-codec corpus with ≥50 refs / ≥5 encoders (per
  Research-0054 §"Open question"), (2) per-frame feature emission
  in Phase A, and (3) clearing v1's 0.95 LOSO PLCC ship threshold
  with a ≥0.005 lift on multi-codec splits per ADR-0235's gate.
  Backlog item: T7-FR-REGRESSOR-V2-PROD.

## References

- Companion research digest: [Research-0054](../research/0058-fr-regressor-v2-feasibility.md).
- Prior ADRs:
  [ADR-0235](0235-codec-aware-fr-regressor.md) (codec-aware decision),
  [ADR-0237](0237-quality-aware-encode-automation.md) (vmaf-tune Phase A),
  [ADR-0249](0249-fr-regressor-v1.md) (v1 baseline),
  [ADR-0040](0040-dnn-session-multi-input-api.md) (multi-input session API),
  [ADR-0041](0041-lpips-sq-extractor.md) (two-input ONNX precedent),
  [ADR-0042](0042-tinyai-docs-required-per-pr.md) (tiny-AI doc bar).
- Schema: `tools/vmaf-tune/src/vmaftune/corpus.py` (`CORPUS_ROW_KEYS`,
  `SCHEMA_VERSION`).
- Source: `req` — user task brief 2026-05-03 ("scaffold
  `fr_regressor_v2` — the codec-aware version of the FR regressor
  that consumes the JSONL corpus emitted by Phase A").

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `ai/scripts/train_fr_regressor_v2.py` — present (scaffold +
  smoke-mode entry point).
- `model/tiny/fr_regressor_v2.{onnx,json,onnx.data}` — registered.
- Doc surface: `docs/ai/models/fr_regressor_v2.md` referenced;
  research digest under `docs/research/`.
- The companion ensemble + probabilistic head landed via ADR-0279
  (this sweep, Accepted).
- Verification command:
  `ls ai/scripts/train_fr_regressor_v2.py model/tiny/fr_regressor_v2.*`.
