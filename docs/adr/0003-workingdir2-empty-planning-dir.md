<!-- markdownlint-disable MD013 MD060 -->
# ADR-0003: Introduce .workingdir2 as new planning directory

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, planning, claude

## Context

The fork already has a populated `.workingdir/` from earlier sessions. A new planning cycle needed its own untouched scratch space so the existing dossier would not be clobbered and so that both generations of planning material remained side-by-side for reference.

## Decision

We will create `.workingdir2/` as a new empty directory alongside the existing `.workingdir/` (now populated with the current planning dossier) rather than overwriting or archiving the old one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reuse `.workingdir/` | One scratch location | Destroys prior planning state; no audit trail | Rejected — prior context is load-bearing |
| Archive `.workingdir/` to `.workingdir.bak` | Keeps single canonical scratch | Requires a move at session start; noisy diff | Parallel directories are simpler |

This decision was a default — no alternatives were weighed beyond the minimal option above.

## Consequences

- **Positive**: old planning artefacts stay readable; new session starts clean.
- **Negative**: two scratch directories present simultaneously; future sessions must know which is canonical.
- **Neutral / follow-ups**: ADR-0019 expands what goes into `.workingdir2/`.

## References

- Source: `Q1.3`
- Related ADRs: ADR-0019
