<!-- markdownlint-disable MD013 MD060 -->
# ADR-1128: Make changelog fragments own release cuts

- **Status**: Accepted
- **Date**: 2026-08-31
- **Deciders**: Lusoris, Codex (OpenAI)
- **Tags**: release, changelog, automation, ci, docs

## Context

VMAFx treats `changelog.d/<section>/*.md` and the legacy archive as the source
of truth for the rendered `CHANGELOG.md` Unreleased block. At the first ordinary
SemVer cut, 1,498 active sources render more than two thousand changelog lines.
Release-please's independent changelog updater can create a version heading, but
it neither consumes those sources nor understands the renderer contract. The
next renderer run would therefore place already-released entries back under
Unreleased.

A release cut must also reject a stale generated PR: all coordinated version
markers must equal the requested version, the rendered block must be current,
and no late fragment may remain unaccounted for.

## Decision

The fragment renderer will be the sole owner of `CHANGELOG.md`, and
release-please will run with `skip-changelog: true`. Before a generated release
PR merges, `scripts/release/rollover-changelog-fragments.sh` will atomically
prepare its changelog cut: validate the exact ordinary SemVer, UTC date, root
manifest and coordinated version markers; require a clean rendered Unreleased
block with at least one active source; add one version heading; remove the
consumed sources; and write a content-hash receipt under
`changelog.d/releases/`. A second identical invocation is a no-op. CI runs both
release-script regression harnesses.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Let release-please update `CHANGELOG.md` | Built into the release action | Leaves 1,498 canonical sources active, so the renderer republishes old entries | Violates the existing fragment ownership contract |
| Keep released fragments and add an exclusion manifest | Preserves every source file at the tip | Adds permanent dual state and makes every render depend on a growing exclusion list | Git history and a compact receipt already preserve provenance |
| Delete fragments manually after publication | No new script | The published release can already contain stale or duplicated notes; the operation is not reproducible | Validation must happen before merge and tag creation |
| Deterministic pre-merge rollover | One source of truth, fail-closed checks, reproducible receipt | The release PR gains one operator finalization step and a large deletion | Chosen; the deletion is reviewable and recoverable from Git |

## Consequences

- **Positive**: every release contains the exact body reviewers saw under
  Unreleased, and a late merge makes the generated release PR fail closed.
- **Negative**: the first rollover deletes a large active fragment set from the
  branch tip, although every source remains recoverable from Git history.
- **Neutral / follow-ups**: the release operator runs the rollover only after
  release-please has updated every coordinated version marker; receipts are not
  active fragments and are ignored by the renderer.

## References

- [ADR-0221](0221-changelog-adr-fragment-pattern.md)
- [Research-1128](../research/1128-fragment-owned-release-cuts.md)
- [release-please manifest configuration](https://github.com/googleapis/release-please/blob/main/docs/manifest-releaser.md)
- Source: `req` — "fix the tags and then bump a release"
- Source: `req` — "all this will be a .x patch of course, not a minor version"
