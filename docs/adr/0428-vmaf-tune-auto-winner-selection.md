<!-- markdownlint-disable MD013 MD060 -->
# ADR-0428: vmaf-tune auto selects one winner

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris maintainers
- **Tags**: vmaf-tune, cli, planning

## Context

`vmaf-tune auto` already composes the Phase F planner, emits per-cell
estimates, and records short-circuits, but the design text still ended with
`pick_pareto(...)` / `realise(winner, ...)` while the JSON output exposed only
an unordered `cells[]` list. Downstream automation therefore had to reimplement
winner selection or assume the first cell was best, which is fragile once
multi-codec and multi-rung plans are enabled.

The first realise step should stay metadata-only in this PR. Starting actual
encodes from `auto` changes runtime, file output, and subprocess error handling;
that needs a separate execution-mode PR. This decision only makes the planner's
single intended cell explicit.

## Decision

`vmaf-tune auto` will select one deterministic winner from the emitted cells,
mark that row with `selected: true`, and record the same decision in
`metadata.winner`. The selector prefers cells that meet both target VMAF and
budget, then cells that meet target VMAF while exceeding budget by the smallest
amount, then the closest quality miss.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep emitting only `cells[]` | No schema change. | Every caller must reimplement selection; impossible to audit a single Phase F decision from the plan alone. | Leaves the backlog gap open. |
| Run encode/score immediately inside `auto` | Fully realises the winner. | Changes runtime from planning to execution, introduces output paths and subprocess failure modes, and overlaps existing corpus/fast surfaces. | Too large for the first closeout PR. |
| Pick first cell after planning | Trivial to implement. | Depends on allow-codec order and rung order rather than the user's target/budget. | Not defensible as a quality-aware default. |

## Consequences

- **Positive**: `auto` JSON now carries a single auditable cell for scripts,
  dashboards, and future encode execution.
- **Negative**: JSON consumers see two new fields, `cells[].selected` and
  `metadata.winner`.
- **Neutral / follow-ups**: A later PR can add an explicit execution mode that
  takes `metadata.winner` and drives encode/score through the existing corpus
  helpers.

## References

- [ADR-0325](0325-vmaf-tune-phase-f-auto.md)
- [docs/usage/vmaf-tune.md](../usage/vmaf-tune.md)
- Source: `req` ("well then go on, #787 has automerge on and i say something when its done, do the next backlog then")
