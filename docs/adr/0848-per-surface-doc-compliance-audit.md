# ADR-0848: Per-Surface Documentation Compliance Audit — Session 2026-05-29

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `docs`, `compliance`, `process`, `per-surface-bar`, `fork-local`

## Context

CLAUDE.md §12 r10 requires that every user-discoverable surface change ship
human-readable docs in the same PR. An audit of the 30 most recent merged
commits on `master` (commits `950d2af51d` through `a4e9e707a0`, PRs #96–#174)
was requested to measure compliance and surface outstanding doc debt.

Three confirmed gaps were found:

1. **PR #47** (Vulkan drop, BREAKING): removed `libvmaf_vulkan.h`, three CLI
   flags, and the `enable_vulkan` meson option but did not update
   `docs/backends/vulkan/overview.md`, `docs/metrics/features.md`, or
   `docs/development/build-flags.md`. All three files still describe Vulkan
   as an operative backend.

2. **PR #87** (VmafLegacyQualityRunner sunset, BREAKING): removed the Python
   runner class but did not add a `docs/development/deprecations.md` entry or
   a migration notice in `docs/usage/python.md`.

3. **PR #135** (log format): removed the `"Error: "` prefix from CUDA error
   log messages — a user-visible output-schema change — without a doc note.

## Decision

Record the three gaps as tracked issues (Issue A, B, C in Research-0848) and
assign follow-up doc-only PRs. No code changes required; the surfaces
themselves are correctly implemented.

The audit methodology (commit-by-commit `--name-only` + surface-type
classification) is the canonical approach for future §12 r10 compliance
sweeps.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Fix gaps inline in this PR | No separate follow-up PRs | Mixes audit deliverable with doc edits; harder to review and bisect | Rejected; the task brief explicitly requests "identify, don't fix" |
| Skip PR #47 (pre-30-commit window) | Fewer findings | Hides highest-severity gap (Vulkan docs are actively misleading) | Rejected; the stale docs are in the current master tree regardless of when the removal landed |

## Consequences

**Positive**: Three concrete follow-up issues filed; Vulkan stale-doc risk
surfaced for immediate action.

**Negative**: Minor — the `docs/backends/vulkan/` pages remain misleading
until Issue A is resolved.

**Neutral follow-ups**:

- Open GitHub issues for Issue A (Vulkan removal docs cleanup) and Issue B
  (VmafLegacyQualityRunner deprecations.md entry).
- ADR citation correction: `docs/metrics/features.md` footnote 6 cites
  ADR-0574 for `aim_score`/`adm3_score` CUDA support; should also cite
  ADR-0746.

## References

- Research-0848:
  `docs/research/research-0848-per-surface-doc-compliance-audit-20260529.md`
- CLAUDE.md §12 r10 (per-surface doc bar)
- ADR-0100: project-wide doc substance rule
- ADR-0726: Vulkan backend removal (PR #47)
- ADR-0749: VmafLegacyQualityRunner sunset (PR #87)
