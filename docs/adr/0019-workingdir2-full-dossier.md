<!-- markdownlint-disable MD013 MD060 -->
# ADR-0019: .workingdir2 is the full planning dossier

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, planning, docs

## Context

ADR-0003 created `.workingdir2/` as an empty directory. The user pushed back on a single-file plan ("is that one file in .workingdir2 really all your findings and analysis and plannings? that's a joke"), requiring a structured planning workspace rather than a stub.

## Decision

`.workingdir2/` is the full planning dossier, organized into README / PLAN / OPEN / analysis / decisions / phases subsections — not a single PLAN.md.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single PLAN.md | Minimal | User rejected; no room for analysis/decisions/phases | Explicitly rejected |
| Structured dossier (chosen) | Room for analysis + decisions + phases + open questions | More files to maintain | User explicitly requested |

This decision was a default — single-file plan was the only alternative and the user rejected it.

## Consequences

- **Positive**: planning state is inspectable per phase and per decision; ADR log (this directory) draws from it.
- **Negative**: more surfaces to keep current; drift risk.
- **Neutral / follow-ups**: session-start discipline (ADR-0028) re-reads the dossier.

## References

- Source: `req` (user: "is that one file in .workingdir2 really all your findings and analysis and plannings? that's a joke")
- Related ADRs: ADR-0003, ADR-0028
