<!-- markdownlint-disable MD013 MD060 -->

# ADR-1198: An unknown `changelog.d/` subdirectory fails the run instead of warning

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: ci, release, docs, testing

## Context

[ADR-0221](0221-changelog-adr-fragment-pattern.md) renders `CHANGELOG.md`'s Unreleased block
from `changelog.d/<section>/*.md`, where `<section>` is a Keep-a-Changelog category.
[ADR-0892](0892-conventional-commits-and-changelog-fragment-hygiene.md) added a guard for
fragments filed under a directory that is not one of those categories, on the grounds that
such fragments are skipped by the renderer. The guard prints a `WARNING` to stderr and
returns success.

That is not a guard. It is a comment that happens to execute. `changelog.d/docs/` exists on
`master` and holds `retrain-runbook-1246.md`, the changelog entry for
[PR #1313](https://github.com/VMAFx/vmafx/pull/1313)'s tiny-AI retrain runbook. It has never
appeared in `CHANGELOG.md` and never would have: the renderer skips it, so `--check` — which
compares rendered output against the committed `CHANGELOG.md` — sees both sides agree the
entry does not exist and reports success. The warning went to stderr in a CI step nobody
reads when the step is green.

The failure mode is silent loss of release notes, which is precisely what the fragment
pattern exists to prevent, and it is invisible to every gate the repository has.

## Decision

`scripts/release/concat-changelog-fragments.sh` will treat a fragment under an unknown
`changelog.d/` subdirectory as an **error**: it prints the offending directory, lists the
files that would be lost, names the valid sections, and returns non-zero. `--check` and
`--write` both fail, so `make lint`, the pre-commit hook and the `Release Script Contract`
CI job all stop on it.

`changelog.d/docs/retrain-runbook-1246.md` moves to `changelog.d/added/`, which restores the
lost entry to the rendered Unreleased block. `docs` is not added as a section: Keep a
Changelog has no such category, and a documentation change that is worth a release note is
an `Added` or `Changed` entry describing the surface it documents.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fail closed on unknown subdirectories (**chosen**) | The guard does what its own comment claims; the loss is impossible to miss; costs one directory rename to adopt | A contributor who invents a directory now gets a hard failure instead of a silent skip — which is the point | — |
| Keep the warning, add a separate CI grep for unknown dirs | No change to the renderer | A second mechanism to keep in sync with the first, and the existing one stays misleading | Two half-guards are worse than one real one |
| Add `docs/` as a valid section | The existing fragment renders unchanged | Keep a Changelog has no `docs` category, and ADR-0221 renders section headings from that fixed set; adding one means inventing a heading downstream tooling does not expect | Fixes one file, leaves the class of bug open |
| Make the renderer render unknown directories into `Changed` | Nothing is ever lost | Silently reclassifying someone's entry is its own surprise, and the section is a deliberate authoring choice | Guessing on the author's behalf |
| Leave it as a warning | Zero work | It already lost a merged PR's release note for the entire life of that PR | The status quo is the bug |

## Consequences

- **Positive**: a misfiled fragment fails at `pre-commit` time with the path in the message,
  rather than vanishing. PR #1313's runbook entry is restored to `CHANGELOG.md`.
- **Negative**: any branch that currently carries a fragment under an unknown directory
  starts failing until it is moved. A search of `master` found exactly one, now fixed.
- **Neutral / follow-ups**: two cases in
  `scripts/release/tests/test-concat-changelog-fragments.sh` pin the behaviour — that the run
  fails, and that it names the files that would be lost. The second one caught a real defect
  in the first draft of this change, where a `2>/dev/null >&2` redirect order sent the file
  listing to `/dev/null`.

## References

- req: found while adding a changelog fragment during the 1.0.0 queue drain; the renderer
  warned about `changelog.d/docs/` and the warning turned out to describe a merged PR's
  lost release note.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — the fragment-rendering pattern.
- [ADR-0892](0892-conventional-commits-and-changelog-fragment-hygiene.md) — introduced the
  warning this ADR promotes to an error.
