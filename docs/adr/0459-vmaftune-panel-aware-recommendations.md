<!-- markdownlint-disable MD060 -->
# ADR-0459: vmaf-tune panel/display-aware recommendation workstream

- **Status**: Proposed
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vmaf-tune, ai, hdr, training, panel, display, fork-local

## Context

The 2026-05-15 HDR/UGC dataset license audit
([Research-0136](../research/0136-hdr-ugc-dataset-license-audit-2026-05-15.md))
surfaced a dataset class the fork has no machinery to consume:
**HDRSDR-VQA** (Chen et al. 2025) ships 22 000+ pairwise JOD scores
across **6 distinct HDR display panels** (OLED + QLED + LCD variants).
That is the only public dataset that cleanly separates *panel-induced*
quality variance from *encode-induced* quality variance.

Today vmaf-tune emits a single CRF / preset recommendation per (target
VMAF, codec) tuple and assumes a generic display. The HDRSDR-VQA pairwise
data lets us learn **panel-conditioned recommendations** — i.e. the
optimal CRF for "85 VMAF on a 2024 OLED" can differ measurably from "85
VMAF on a 2020 QLED" for the same source.

## Decision

Stand up a **panel-aware recommendation extension** for vmaf-tune as a
dedicated workstream tracked under this ADR. Concrete scope:

1. Ingest the 31 open-sourced HDRSDR-VQA clips + JOD scores via a new
   adapter `ai/scripts/hdrsdr_vqa_to_corpus_jsonl.py`.
2. Add a `panel_class` enum column to the corpus schema covering the
   6 HDRSDR-VQA display panels (OLED-2024, OLED-2022, QLED-2024,
   QLED-2022, LCD-2024, LCD-2022 — exact labels TBD per the paper).
3. Train a sibling `vmaftune_panel_predictor_v1` ONNX that maps
   (canonical-6 features + panel_class one-hot + codec ENCODER_VOCAB)
   → CRF delta vs. the base panel.
4. Surface `vmaf-tune --panel oled-2024 ...` CLI flag that loads the
   panel predictor and applies the per-panel CRF delta to the base
   recommendation.
5. Document the limit: 31 open-sourced clips is small for a robust
   model; the head ships as `Status: Proposed` until the gate clears
   on a held-out subset.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Stand up the workstream (this ADR)** | Closes the only public dataset class the fork can't currently consume; differentiates the fork on a real-world axis | Small training corpus (31 clips); gate threshold is uncertain | Chosen — the differentiation value is high, and the dataset is the only public source for this signal |
| **Skip panel-awareness; rely on display-agnostic VMAF** | Smallest implementation surface | Loses the only dataset axis we have for panel variance; defers to client-side calibration which we have no control over | Rejected — leaves a measurable accuracy gap on real-world consumer displays |
| **Fold panel-class into the existing fr_regressor_v3 head** | No new ONNX | fr_regressor_v3 predicts VMAF, not CRF deltas; conflating the two would muddy both heads | Rejected — separate concerns, separate heads |
| **Wait for a larger panel-pairwise corpus** | More robust gate | No public dataset of comparable scale exists; YouTube SFV+HDR is opaque on access | Rejected — 31 clips is enough to scaffold the pipeline; expansion is a follow-up if/when more data arrives |

## Consequences

### Positive

- vmaf-tune gains a panel-aware recommendation surface that no
  open-source competitor offers.
- Establishes the data-ingestion pattern for future panel-pairwise
  corpora (LIVE HDR's ambient-condition data, future YouTube SFV+HDR
  data when access lands).

### Negative

- Adds a 7th model to the registry (`vmaftune_panel_predictor_v1`)
  with attendant maintenance burden.
- 31-clip training set is small; gate may not clear immediately.
  Status stays `Proposed` until it does.

### Neutral

- HDRSDR-VQA ingestion is its own adapter, kept separate from the
  KonViD-150k / CHUG paths so the panel-class column doesn't pollute
  the existing corpora.

## References

- [Research-0136](../research/0136-hdr-ugc-dataset-license-audit-2026-05-15.md)
  — HDR/UGC dataset license audit (this ADR's parent research).
- HDRSDR-VQA — Chen et al. 2025
  (<https://live.ece.utexas.edu/research/Bowen_SDRHDR/sdr-hdr-bowen.html>).
- ADR-0325 — KonViD-150k corpus ingestion (the existing
  separate-adapter pattern this ADR mirrors).
- ADR-0336 — KonViD MOS head (the model-registration pattern this
  ADR mirrors).
