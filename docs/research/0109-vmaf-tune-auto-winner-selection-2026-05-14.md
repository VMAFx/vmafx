<!-- markdownlint-disable MD060 -->
# Research-0109: vmaf-tune auto winner selection

- **Date**: 2026-05-14
- **Area**: vmaf-tune Phase F
- **Related ADR**: [ADR-0428](../adr/0428-vmaf-tune-auto-winner-selection.md)

## Question

What is the smallest real code change that moves `vmaf-tune auto` beyond
plan-only cell emission without conflating the planner with encode execution?

## Findings

- `tools/vmaf-tune/src/vmaftune/auto.py` already emits one cell per
  `(rung, codec)` with estimated VMAF, estimated bitrate, CRF, HDR args,
  confidence decision, sample-clip propagation, and recipe metadata.
- The module docstring and usage docs still describe a final
  `pick_pareto(...); return realise(winner, ...)` step, but the JSON schema
  exposed no winner.
- `recommend.pick_target_vmaf` and the corpus coarse-to-fine helper both
  preserve quality first: return a concrete closest miss when no row clears
  the target rather than returning an empty result.
- Actual encode execution belongs to the existing corpus/encode/score seams.
  Starting that from `auto` would add output-path and subprocess semantics
  that are larger than the current backlog closeout.

## Resulting implementation

The planner now performs a deterministic estimated-row selection:

| Case | Winner rule |
|---|---|
| At least one cell meets target and budget | Lowest estimated bitrate; tie-break by higher VMAF, higher rung, codec, original index. |
| No in-budget quality pass, but at least one target pass | Smallest budget overage; tie-break by lower bitrate, higher VMAF, higher rung, codec, original index. |
| No target pass | Highest estimated VMAF; tie-break by lower bitrate, higher rung, codec, original index. |

The result is recorded in `metadata.winner`, and cells are annotated with
`selected: true|false`.

## Validation

- Unit tests cover all three winner statuses.
- Existing smoke JSON tests assert the selected cell marker and winner metadata
  round-trip through `emit_plan_json`.
