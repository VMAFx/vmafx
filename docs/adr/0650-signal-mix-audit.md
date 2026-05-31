<!-- markdownlint-disable MD013 MD060 -->
# ADR-0650: Add a Signal-Mix Audit CLI

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, metrics, audit, hdr

## Context

The fork now has multiple overlapping VQA signal sources: Netflix canonical
features, full-feature parquet tables, subjective MOS corpora, DNN perceptual
models, saliency models, encoder profile metadata, and HDR-specific CHUG rows.
Several older audits catalogued feature availability, but they did not give an
operator a current, table-driven answer to "what signal family is missing?" or
"which not-yet-wired metric could add value through intersection with the
current stack?"

This matters because the next product gains are unlikely to come from one more
canonical-six retrain. HDR, panel-aware recommendations, saliency-weighted
encodes, texture metrics, codec-specific profiles, and MOS heads each need a
clear signal map before model promotion.

## Decision

We will ship `ai/scripts/signal_mix_audit.py` as a table-only diagnostic CLI.
It reads parquet/JSONL/JSON feature tables, classifies columns into VQA signal
families, computes target correlations, flags redundant pairs, surfaces
cross-family complementary intersections, and renders JSON plus Markdown
reports with missing/weak signal-family recommendations.

The CLI is advisory and side-effect free. It does not extract features, train
models, mutate corpus files, or gate CI by default.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend `feature_correlation.py` | Reuses an existing script | That script is intentionally narrow: pairwise correlations and sklearn importances for one parquet target | Rejected; the new question is signal-family coverage and candidate intersections, not just ranking columns |
| Keep documenting audits manually | Fast for one session and flexible for narrative notes | Goes stale immediately after refreshes and does not inspect generated tables | Rejected; stale speed-feature notes already showed why prose-only audits are unreliable |
| Build the full continuous feature-mix evaluator now | Most complete long-term answer | Larger scope: YAML grids, subset search, model fitting, uncertainty intervals, and cost weighting | Deferred; this PR gives a safe first diagnostic layer while longer evaluator work remains separate |
| Add table-only signal-mix audit | Cheap, deterministic, can run on current local tables, highlights missing metric families and candidate intersections | Heuristic column-family mapping can miss custom names until updated | Accepted |

## Consequences

- **Positive**: model-refresh and HDR work can see missing signal families before
  burning compute on a retrain.
- **Positive**: the report explicitly calls out candidate metrics that are not
  yet wired, such as U2NetP, DISTS, HDR-VDP, DOVER, Q-Align, and panel metadata.
- **Positive**: stale prose audits become inputs to compare against, not the
  source of truth for current table state.
- **Negative**: family matching is name-based. New custom column names may need
  new regexes before the audit classifies them correctly.
- **Negative**: geometry/content metadata is only conditioning context; it can
  explain MOS variation but does not replace perceptual metrics.
- **Neutral / follow-ups**: the continuous feature-mix evaluator can consume
  this audit's findings later, but remains a separate modelling/evaluation
  surface.

## References

- [Research-0026](../research/0026-cross-metric-feature-fusion.md) — earlier
  cross-metric feature-fusion rationale.
- [continuous-feature-mix-evaluation-design-2026-05-18](../research/continuous-feature-mix-evaluation-design-2026-05-18.md)
  — broader evaluator design this diagnostic does not try to replace.
- Source: req: "well and in this audit perhaps find gaps that we have no metric/signal for at all or so"
- Source: req: "yeah every possible gain through intersection (even of not yet included metrics)... thats an interesting topic for sure lol"
