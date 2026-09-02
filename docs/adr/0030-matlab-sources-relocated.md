<!-- markdownlint-disable MD013 MD060 -->
# ADR-0030: Relocate MATLAB sources under python/vmaf/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, matlab, python

## Context

`ROOT/matlab/` (strred, SpEED, STMAD_2011_MatlabCode, cid_icid — third-party reference implementations run via the matlab harness) is consumed by exactly one Python file (`matlab_feature_extractor.py`). Its presence at repo root clutters external discoverability. Same cleanup rationale as ADR-0029.

## Decision

Move `ROOT/matlab/` to `python/vmaf/matlab/`. Update `matlab_feature_extractor.py`'s `MATLAB_WORKSPACE = VmafConfig.root_path("matlab", …)` calls to `VmafConfig.root_path("python", "vmaf", "matlab", …)`. No env override — these are static resources, not scratch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep at root | No churn | Root stays noisy | Rejected |
| Move under `python/vmaf/matlab/` (chosen) | Co-located with single consumer | Path fixups | Matches family pattern |

Rationale: same as ADR-0029 — scratch/data trees consumed exclusively by the Python harness do not belong at the repo root.

## Consequences

- **Positive**: repo root shrinks; MATLAB resources live with their only consumer.
- **Negative**: matlab harness docs must reflect new paths.
- **Neutral / follow-ups**: no env override — static resources.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0029, ADR-0038
