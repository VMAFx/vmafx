<!-- markdownlint-disable MD013 MD060 -->
# ADR-0675: MOS Label Materializer Batch Manifest

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, mos, materializer, provenance, fork-local

## Context

[ADR-0663](0663-mos-label-materializer.md) made MOS labelling an explicit
table-side materialization step. That fixed the bad trainer behaviour where a
real-looking KoNViD MOS-head output could be produced from a synthetic fallback
after the input feature table had no MOS labels.

The next refresh wave has multiple feature tables that need the same explicit
join: KoNViD, CHUG/HDR variants, UGC, BVI, and later panel/profile subsets.
Running the single-table script repeatedly by hand is fragile because each
join has coverage thresholds, key regexes, overwrite policy, and audit paths
that downstream trainers must be able to cite.

## Decision

Add `ai/scripts/batch_materialize_mos_labels.py`, a manifest-driven
orchestrator over `materialize_mos_labels.materialize()`. The manifest has
shared `defaults` and a `tables[]` array. Each table carries `id`, `features`,
`labels`, `out`, optional `audit_json`, and any single-table join override.
Relative paths resolve from the manifest directory unless `--base-dir` is
supplied.

The batch runner writes each labelled feature table, optional per-table audit
JSON, and a `mos-label-materializer-batch-v1` report with ADR-0661 run
provenance. It must not parse MOS rows itself and must not invoke training.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Manifest-driven batch wrapper over the shared materializer | Repeatable multi-table MOS joins; one provenance report; preserves ADR-0663 semantics | Adds one operator-facing CLI | Chosen: it makes the label refresh replayable without duplicating join logic |
| Keep shell loops | No new Python surface | No durable batch artifact, weak provenance, easy threshold/path drift | Rejected: shell history is not training evidence |
| Join labels inside trainers | Fewer operator commands for one model | Reintroduces per-trainer key and match-rate policy drift | Rejected: ADR-0663 intentionally moved labels to table materialization |
| Add corpus-specific label scripts | Simple per-corpus defaults | Duplicates column inference and overwrite rules | Rejected: corpus differences are already manifest entries |

## Consequences

- **Positive**: MOS-labelled refresh tables can be regenerated from one batch
  manifest and cited by training, audits, and model cards.
- **Negative**: Join option changes must keep the single-table materializer
  and batch manifest validation in sync.
- **Neutral / follow-ups**: Run the batch manifest on refreshed KoNViD,
  CHUG/HDR, UGC, and BVI tables, then rerun MOS-head and signal-mix training
  with explicit label provenance.

## References

- [ADR-0663](0663-mos-label-materializer.md) — table-side MOS label
  materialization.
- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared AI run provenance.
- [Research-0695](../research/0695-mos-label-materializer-batch-manifest.md)
  — implementation digest.
- Source: req — "so all, netflix, regressors, encoders etc... everything we did so far needs updates"
- Source: req — "go on with next backlog?"
