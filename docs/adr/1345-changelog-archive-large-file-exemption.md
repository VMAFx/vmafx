<!-- markdownlint-disable MD013 MD060 -->
# ADR-1345: Exempt release changelog archives from the large-file gate

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: ci, release, docs

## Context

The pre-commit `check-added-large-files` hook refuses any added file above 1,024 KB. [ADR-1128](1128-fragment-owned-release-cuts.md) makes the release PR's changelog cut, `scripts/release/rollover-changelog-fragments.sh`, delete every fragment it consumes. When the rendered body is longer than `--archive-over` lines (400 by default), the full text moves to `docs/changelog-archive/X.Y.Z.md`, and `CHANGELOG.md` keeps a per-section index; that archive shape comes from [ADR-1233](1233-github-native-release-notes.md).

The first release candidate consumes the fork's entire fragment history, every fragment since the fork began (2,035 when this was found). That archive is 1.86 MB of text, so the cut commit on release PR #1213 was refused and 1.0.0-rc.1 could not be cut.

[ADR-1115](1115-brisque-nr-metric.md) met the same gate with a 2.1 MB generated C header and kept the limit by embedding the model at build time (a generated `.c` file) instead of committing the header. That route does not exist here. After the cut, the archive is the only remaining copy of the consumed fragments' text, and the rollover receipt pins the SHA-256 of the rendered body the archive holds verbatim below its header.

## Decision

`check-added-large-files` excludes `^docs/changelog-archive/[^/]+\.md$`. Only top-level Markdown files in that directory are exempt. Every other path, including binaries or nested files placed under `docs/changelog-archive/`, keeps the 1,024 KB limit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Exempt the archive directory's Markdown files (chosen) | One narrow pattern; one file holds the whole body the receipt hashes | One generated text path is outside the size guard | The file is written once per release by a reviewed script, and later releases are far smaller |
| Split the archive into files under 1 MB | Keeps the global limit untouched | More rollover code and tests, several index links per release, a receipt spanning files | More moving parts to avoid an exemption for text the repository must keep anyway |
| Raise `--maxkb` for the whole repository | One-line change | Weakens the guard for binaries, models and fixtures everywhere | The problem is one generated path, not the limit |

## Consequences

- **Positive**: a release cut whose archive exceeds 1 MB can be committed, so 1.0.0-rc.1 can be cut and published.
- **Negative**: a very large file placed at `docs/changelog-archive/<name>.md` by hand would not be caught. The rollover is the only writer the release guide documents.
- **Neutral / follow-ups**: `scripts/release/tests/test-rollover-changelog-fragments.sh` (T18) pins the exact exclusion pattern, checks that the archive path the rollover writes matches it, and checks that neighbouring paths do not, so moving the archive path or editing the pattern fails a test until both change together.

## References

- Popup, 2026-09-27: "The RC1 changelog archive (docs/changelog-archive/1.0.0-rc.1.md, 1.86 MB of generated text from 2,035 fragments) exceeds the repo's 1,024 KB check-added-large-files limit, so the cut commit is refused. How should I resolve it?" Answer: "Exempt the archive dir (Recommended)".
- [ADR-1128](1128-fragment-owned-release-cuts.md) — fragment-owned release cuts and the rollover.
- [ADR-1233](1233-github-native-release-notes.md) — the `--archive-over` archive and its per-section index.
- [ADR-1115](1115-brisque-nr-metric.md) — the earlier large-file case, solved by build-time generation.
- [ADR-1201](1201-release-candidates-before-1-0-0.md) — release candidates before 1.0.0.
