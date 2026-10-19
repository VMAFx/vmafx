<!-- markdownlint-disable MD013 MD060 -->
# ADR-0636: Tombstone: ADR number claimed from the allocator and never used

- **Status**: Deprecated
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `tombstone`, `process`

## Context

ADR-0637 mentions an `ADR-0636 stub`: an agent claimed this number with
`scripts/adr/next-free.sh --claim` and its work was left partly prepared.

## Decision

No decision was recorded under this number and none is needed: the master CI
repair that the claim was meant for is recorded in ADR-0637. The number is
kept so it is never reused (ADR numbers are not reused).

This record was written on 2026-10-06 from the commits and ADRs named below,
because the number is cited and no file existed. It records what happened; it
decides nothing new.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave the citation unresolved | No new file | Readers of the citing ADR find nothing | Rejected by the maintainer in the ADR audit of 2026-10-06 |
| Rewrite the citing ADR | Points at the real record | Bodies of Accepted ADRs are frozen | The citing ADR gets an errata block instead |

## Consequences

- **Positive**: the citation resolves and the history is reachable.
- **Negative**: one more short record.
- **Neutral / follow-ups**: none.

## References

- ADR-0637 (fix 5 master CI failures)
- Source: the ADR audit of 2026-10-06, maintainer decision to file a short record for each cited number that had no file.
