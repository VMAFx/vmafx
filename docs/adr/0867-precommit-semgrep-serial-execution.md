<!-- markdownlint-disable MD013 MD060 -->
# ADR-0867: Tombstone: run the semgrep-local pre-commit hook serially

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `tombstone`, `ci`, `pre-commit`, `semgrep`

## Context

ADR-0893 cites ADR-0867 as the in-flight record for adding `require_serial:
true` to the `semgrep-local` hook, because parallel invocations exhausted
`io_uring` memlock. No file was filed.

## Decision

The change landed as #487 (`26452fe6a`, 2026-05-31) and
`.pre-commit-config.yaml` keeps `require_serial: true` on the hook.

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

- ADR-0893
- PR #487, commit `26452fe6a`
- Source: the ADR audit of 2026-10-06, maintainer decision to file a short record for each cited number that had no file.
