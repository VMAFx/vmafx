# ADR-1242: Require source-owned ADR metadata freshness

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Kilian, Codex
- **Tags**: docs, ci, adr, navigation, automation

## Context

ADR-0937 introduced generated navigation and tag indexes with check modes,
but make and CI never ran those checks. At master `7bafbb8cc`, every
existing tag output differed from regeneration, 145 tag pages were absent,
and navigation contained duplicate and missing ADR entries. The tag
renderer also dropped lint headers introduced in a later cleanup.
Fragment concatenation passed despite a missing ADR-1123 row and five
broken secondary ADR references in mutable fragment descriptions.
A strict MkDocs build cannot prove these metadata sets are complete.

## Decision

Make `docs-fragments-check` validate ADR fragment coverage and references,
then compare changelog, ADR index, tags, and navigation against their
sources. Make the corresponding write target render tags before navigation.
Run the check in local pre-commit, required Docs CI, and Pages builds.
Test generator determinism, read-only checks, invalid inputs, and sentinel
protection. Refresh generated output from existing ADR titles and tags;
correct only verified mutable index references. Keep accepted ADRs intact.

## Alternatives considered

| Option | Benefit | Cost or reason rejected |
| --- | --- | --- |
| Shared make targets plus checks | Same source contract locally and in CI | Metadata regeneration becomes required; chosen |
| One-time regenerated files | Immediate cleanup | Drift recurs without enforcement |
| MkDocs strict alone | Existing render gate | Cannot detect absent generated pages or stale membership |
| Rewrite accepted ADR metadata | Simplifies old references | Changes archival source meaning; rejected |

## Consequences

Generated tables retain their layout lint directives. The renderer escapes
ordinary underscores, HTML-like text, and table pipes so titles render
faithfully; meaningful spaces inside code spans are preserved and only
those generated pages carry the MD038 exception. Tag names remain unchanged,
but unsafe output basenames and the reserved index filename fail clearly.
Each ADR contributes at most one row per normalized tag.

Tag output is fully staged before individual atomic replacements; obsolete
pages are removed afterward. Navigation requires exactly one ordered
sentinel pair and preserves bytes outside it. An interrupted multi-file
refresh may be partial and is detected by the next check.
Pages deployment also requires a docs artifact-producing build; a code-only
impact skip no longer starts a deploy with no artifact. Existing MkDocs
INFO-level archival/source-link policies remain unchanged.

## References

- `req`: "if you find bugs/whatever, just fix them, its not out of scope".
- [ADR-0937](0937-mkdocs-nav-decade-buckets.md): generated metadata ownership.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md): fragment pattern.
- [Research-1242](../research/1242-generated-adr-freshness.md): drift evidence,
  reference provenance, and validation.
