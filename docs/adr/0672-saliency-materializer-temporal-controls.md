<!-- markdownlint-disable MD013 MD060 -->
# ADR-0672: Saliency Materializer Temporal Controls

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, saliency, materializer, provenance, fork-local

## Context

[ADR-0655](0655-saliency-feature-materializer.md) added
`ai/scripts/materialize_saliency_features.py` so refreshed AI tables can
carry `saliency_mean` and `saliency_var` before predictor and MOS-head
retraining. The first implementation always called the saliency helper with
the historical mean reducer.

That was too weak for the current signal-mix work. `vmaf-tune` already exposes
`mean`, `ema`, `max`, and `motion-weighted` temporal saliency reducers, and
[ADR-0671](0671-u2netp-mirror-exporter.md) made alternate saliency models
experimentally buildable. If the materializer writes only anonymous
`saliency_*` columns, later training runs cannot tell whether a row came from
`saliency_student_v1`, `saliency_student_v2`, U2NetP, mean aggregation, or an
EMA experiment.

## Decision

Expose temporal saliency controls on the table materializer and make newly
materialized rows self-describing. The script now accepts
`--temporal-aggregator`, `--ema-alpha`, and `--model-id`, passes the temporal
controls into `vmaftune.saliency.compute_saliency_map()`, and records
`saliency_model_id`, `saliency_aggregator`, and `saliency_ema_alpha` on rows
that it computes. Existing rows skipped because they already contain finite
saliency values keep their existing metadata; the materializer must not invent
provenance for older anonymous columns.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add temporal/model controls plus row metadata | Makes saliency-enriched tables comparable across model and reducer experiments; reuses the tested `vmaf-tune` saliency reducers | Adds three small metadata columns to newly materialized tables | Chosen: it closes the immediate AI-table attribution gap without running a new model |
| Keep the materializer mean-only | Preserves the smallest CLI surface | Blocks EMA/max/motion-weighted saliency experiments and hides the reducer identity in derived tables | Rejected: the current audit specifically needs richer saliency intersections |
| Put model/reducer identity only in the audit JSON | Avoids repeated row-level metadata | Tables can be detached from audits during joins and retraining; downstream scripts must carry sidecars perfectly | Rejected: row-level attribution is safer for long-lived local training artifacts |
| Create a separate materializer for U2NetP | Lets each model own custom knobs | Duplicates table IO, decode, failure-status, and provenance logic | Rejected: model selection is a parameter of the same saliency feature family |

## Consequences

- **Positive**: Saliency materialization can now compare image-student,
  U2NetP, EMA, max, and motion-weighted runs without changing downstream
  feature-table readers.
- **Negative**: New saliency tables contain three extra metadata columns
  unless the operator explicitly disables them.
- **Neutral / follow-ups**: Run the materializer over refreshed CHUG, KoNViD,
  UGC, Netflix, and BVI tables with explicit model ids, then rerun the
  signal-mix audit and retrain candidate MOS/predictor heads.

## References

- [ADR-0655](0655-saliency-feature-materializer.md) — original table
  materializer.
- [ADR-0650](0650-signal-mix-audit.md) — signal-mix audit that consumes
  saliency evidence.
- [ADR-0396](0396-video-saliency-extension.md) — temporal saliency reducer
  motivation.
- [ADR-0671](0671-u2netp-mirror-exporter.md) — alternate U2NetP saliency
  model export path.
- [Research-0692](../research/0692-saliency-materializer-temporal-controls.md)
  — implementation digest.
- Source: req — "well that means do u2netp (experimental)... we can only learn i guess lol"
- Source: req — "well and in this audit perhaps find gaps that we have no metric/signal for at all or so"
