<!-- markdownlint-disable MD013 MD060 -->
# ADR-0979: Tombstone: narrowing markdownlint to five rules, rejected

- **Status**: Superseded by [ADR-0980](0980-markdown-lint-full-ruleset-discharge.md)
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `tombstone`, `docs`, `lint`, `markdown`

## Context

ADR-0980 records that ADR-0979 (proposed in PR #497) would have reduced
`.markdownlint.json` from about 36 rules to a five-rule blank-line subset,
which silently disables about 31 rules tree-wide. No file was filed.

## Decision

The narrowing was rejected by the maintainer and replaced by ADR-0980, which
discharged the whole rule set instead.

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

- ADR-0980 (markdown-lint-full-ruleset-discharge)
- Source: the ADR audit of 2026-10-06, maintainer decision to file a short record for each cited number that had no file.
