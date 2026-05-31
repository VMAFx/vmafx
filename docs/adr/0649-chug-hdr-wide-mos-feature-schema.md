<!-- markdownlint-disable MD060 -->
# ADR-0649: CHUG HDR Wide MOS Feature Schema

- **Status**: Proposed
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, hdr, chug, mos, training

## Context

ADR-0648 added the CHUG-named HDR MOS trainer entry point, but the first
local probes still used the existing 11-column KonViD MOS feature layout:
canonical-6 means, saliency placeholders, and TransNet shot placeholders.
On the completed CHUG feature shards, that layout discards most of the
materialised signal: per-feature temporal p10/p90/std aggregates and HDR
ladder / geometry metadata.

The first local CHUG probes missed the production correlation gate even with
longer training:

- 30 epochs: PLCC `0.7961`, SROCC `0.7822`, RMSE `0.3143`.
- 120 epochs: PLCC `0.8243`, SROCC `0.8139`, RMSE `0.2913`.
- 300 epochs: PLCC `0.8276`, SROCC `0.8140`, RMSE `0.2892`.

That plateau is not enough evidence that CHUG itself is weak; it is evidence
that the first 11-column baseline was too narrow for an HDR bitrate-ladder
corpus.

## Decision

Add a named CHUG-local feature schema, `chug-hdr-wide-v1`, and make
`ai/scripts/train_chug_hdr_mos_head.py` use it by default.

The schema is 34 columns:

- canonical-6 means: `adm2`, `vif_scale0..3`, `motion2`;
- p10, p90, and standard deviation for each canonical feature;
- CHUG HDR ladder / geometry metadata: bitrate in Mbps, portrait flag,
  reference-row flag, distorted/reference dimensions, duration, feature frame
  count, and bit-depth normalisation.

The existing KonViD schema remains `konvid-v1` and keeps its 11-column order.
The CHUG wrapper exposes `--feature-schema konvid-v1` only for ablation and
regression comparisons.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the 11-column baseline | Smallest patch; directly comparable to KonViD MOS head | Throws away CHUG temporal/HDR metadata; the first plateau could be a self-inflicted feature bottleneck | Rejected by user direction to widen the features and because the data is already available |
| Replace `FEATURE_COLUMNS` globally | One constant; fewer code paths | Silently invalidates the committed `konvid_mos_head_v1.onnx` predictor contract | Rejected — existing model compatibility is load-bearing |
| Add a named CHUG schema | Uses available CHUG signal while preserving KonViD compatibility | Adds schema selection plumbing and a second manifest layout | Chosen as the minimal safe widening |
| Train a new architecture before widening features | Might fit nonlinear HDR effects better | Makes it impossible to tell whether the bottleneck was features or model shape | Deferred until the wide-schema baseline is measured |

## Consequences

- **Positive**: CHUG HDR MOS experiments now consume the signal already emitted
  by `chug_extract_features.py`.
- **Positive**: The committed KonViD MOS model remains byte-compatible because
  its `konvid-v1` feature order is unchanged.
- **Negative**: Local CHUG manifests can now have different input width from
  `konvid_mos_head_v1`; downstream experiment scripts must read
  `feature_schema` / `feature_order` from the manifest instead of assuming 11
  columns.
- **Neutral / follow-ups**: If the wide schema still misses the gate, the next
  experiments are CHUG-specific head shape, panel/display features, and
  U2NetP/saliency-derived features. Do not lower the gate just because this
  baseline misses.

## References

- [ADR-0648](0648-mos-head-feature-jsonl-chug-id.md)
- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [ADR-0427](0427-chug-hdr-feature-materialisation.md)
- Source: `req` — "no widen them"
- Source: `req` — "we need them, didnt say to stop anything"
- Source: `req` — "so means we need both anyways lol"
