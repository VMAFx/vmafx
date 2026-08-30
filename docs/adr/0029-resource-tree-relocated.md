<!-- markdownlint-disable MD013 MD060 -->
# ADR-0029: Relocate resource tree under python/vmaf/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, python, docs

## Context

`ROOT/resource/` (example datasets, param files, model-training params, tutorial images) is consumed exclusively by the Python harness, but sits at the repo root where external consumers see it first. User: "some project rood dirs should be cleaned up/moved as well".

## Decision

Move `ROOT/resource/` to `python/vmaf/resource/`. Resolve via a `RESOURCE` constant in `config.py`, overridable with `VMAF_RESOURCE` env var; `VmafConfig.resource_path()` routes through it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep at repo root | No code churn | Root stays noisy | Rejected per the ADR-0029..0034 family |
| Move to `python/vmaf/resource/` (chosen) | Co-located with consumer | Path updates required | Matches ADR-0026 pattern |

Rationale (from rationale note on the cleanup family): scratch/data trees consumed exclusively by the Python harness do not belong at the repo root.

## Consequences

- **Positive**: repo root shrinks; discoverability improves for new contributors.
- **Negative**: all `VmafConfig.resource_path()` callers routed through the new constant.
- **Neutral / follow-ups**: env override retained for alternate layouts.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0026, ADR-0030, ADR-0031, ADR-0032, ADR-0033, ADR-0034
