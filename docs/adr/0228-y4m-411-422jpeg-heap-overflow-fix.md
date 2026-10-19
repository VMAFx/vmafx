<!-- markdownlint-disable MD013 MD060 -->
# ADR-0228: Tombstone: heap-buffer-overflow fix in the Y4M 411 to 422jpeg conversion

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: lusoris
- **Tags**: `tombstone`, `y4m`, `fuzz`, `correctness`

## Context

ADR-0382 cites ADR-0228 for a one-byte heap-buffer-overflow in
`y4m_convert_411_422jpeg` found by the libFuzzer harness. No file was ever
filed under this number.

## Decision

The fix landed without a record of its own: PR #357 (`05ba29a6c`, 2026-05-05)
added the `(x << 1 | 1) < dst_c_w` guard to the first and second sub-loops,
the one the third sub-loop already carried. The related decision to reject
non-positive widths and heights is ADR-0382.

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

- ADR-0382 (y4m-neg-dimension-rejection)
- PR #357, commit `05ba29a6c`
- Source: the ADR audit of 2026-10-06, maintainer decision to file a short record for each cited number that had no file.
