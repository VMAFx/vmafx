<!-- markdownlint-disable MD013 MD060 -->
# ADR-0674: Second-Opinion Materializer Batch Manifest

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, second-opinion, materializer, provenance, fork-local

## Context

[ADR-0657](0657-second-opinion-feature-materializer.md) added
`ai/scripts/materialize_second_opinion_features.py`, a table-side joiner for
externally generated NR/MOS scorer JSON. That design intentionally keeps
competitor execution out of the repo: operators generate DOVER, Q-Align,
FAST-VQA, fork-NR, or other sidecars elsewhere, then join the scalar evidence
onto refreshed feature tables.

The current signal-mix backlog needs those joins across several refreshed
tables. Repeating the single-table command by hand risks mixing scorer labels,
missing policies, and output paths, and it produces no single artifact that
downstream retraining can cite as the second-opinion refresh set.

## Decision

Add `ai/scripts/batch_materialize_second_opinion_features.py`, a manifest-driven
orchestrator over the existing second-opinion materializer. The manifest has
shared `defaults` and a `tables[]` array. Each table carries `id`, `features`,
`scores`, `out`, optional `audit_json`, and any single-table join override.
Relative paths resolve from the manifest directory unless `--base-dir` is
supplied.

The batch runner writes each joined table, optional per-table audit JSON, and a
`second-opinion-materializer-batch-v1` report with ADR-0661 run provenance. It
must call `materialize_second_opinion_features.materialize()` for every table;
it does not parse scorer payloads itself and does not invoke external scorer
binaries.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Manifest-driven batch wrapper over the shared joiner | Repeatable multi-table joins; one provenance report; preserves the no-external-scorer boundary | Adds one operator-facing CLI | Chosen: it closes the execution gap without changing row semantics |
| Keep shell loops | No new Python surface | No stable batch artifact, weak provenance, easy label/path drift | Rejected: shell history is not adequate training evidence |
| Invoke competitor scorers from the batch runner | Fully automated end-to-end scorer generation | Vendors/links third-party competitors and violates ADR-0657's table-side boundary | Rejected: scorer execution remains outside this repo |
| Add corpus-specific second-opinion scripts | Simple per-corpus defaults | Duplicates join policy and column semantics | Rejected: corpus differences are already expressible in manifest entries |

## Consequences

- **Positive**: Second-opinion joins for CHUG, KoNViD, UGC, Netflix, and BVI
  can be replayed from one manifest and cited by retraining jobs.
- **Negative**: Join option changes must keep the single-table script and batch
  manifest validation in sync.
- **Neutral / follow-ups**: Generate scorer sidecars, run the batch manifest on
  refreshed tables, rerun the signal-mix audit, and measure MOS/predictor
  retrain impact.

## References

- [ADR-0657](0657-second-opinion-feature-materializer.md) — table-side
  second-opinion joiner.
- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared AI run provenance.
- [Research-0694](../research/0694-second-opinion-materializer-batch-manifest.md)
  — implementation digest.
- Source: req — "yeah every possible gain through intersection (even of not yet included metrics)... thats an interesting topic for sure lol"
- Source: req — "well go on i guess we have enough backlog..."
