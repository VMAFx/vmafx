<!-- markdownlint-disable MD018 MD036 -->
# ADR-0221: CHANGELOG + ADR-index fragment-file pattern

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Opus 4.7 1M-ctx)
- **Tags**: process, release, docs, ci, fork-local

## Context

Every PR in flight during the 2026-04-28 → 2026-04-29 sprint fought merge
conflicts in two consolidated files:

- `CHANGELOG.md` — every PR adds a bullet to the Unreleased section under
  one of the Keep-a-Changelog headings (`Added`, `Changed`, etc.).
- `docs/adr/README.md` — every PR adds one row to the index table.

When two PRs branch off the same merge base and each adds its own row, the
merge in the second PR fails because both touch the same `### Added`
sub-section header line or the same end-of-table row. Sister PRs #195,
#202, #190, #194, #193, #181 all required manual rebase passes solely to
move bullets and rows past one another. The actual code review on each
ran in seconds; the merge bookkeeping cost minutes.

Fragment files are the standard fix. Each PR adds **a new file** under a
fragment directory; a release-time concatenator renders the consolidated
output. New files don't collide because their paths differ — each PR
chooses a unique fragment filename keyed by its task ID.

`release-please` (already wired for the fork) supports `extra-files`
hooks and a CI-side regeneration step. `changie` is the off-the-shelf
tool but adds a Go binary dependency for a problem that fits in 80 lines
of bash.

## Decision

Adopt **in-tree shell-script fragment concat** for both surfaces:

- `changelog.d/{added,changed,deprecated,removed,fixed,security}/*.md`
  — one Markdown file per PR, lexically sorted within section.
  `scripts/release/concat-changelog-fragments.sh` renders the
  `Unreleased` body of `CHANGELOG.md`. `--check` is the CI lane;
  `--write` is the release-please / local pre-push call.
- `docs/adr/_index_fragments/NNNN-slug.md` — one Markdown table row per
  ADR, keyed by full ADR slug (handles the legacy `0199-...` collision).
  `scripts/docs/concat-adr-index.sh` renders `docs/adr/README.md` driven
  by a frozen `_order.txt` manifest that preserves the existing
  commit-merge order.

Migration is content-preserving:

- The existing 3119-line `Unreleased` body is archived **verbatim** as
  `changelog.d/_pre_fragment_legacy.md`. The concatenator emits the
  archive first, then any per-section fragments. New PRs add fragments
  only; the archive is rewritten only when release-please cuts a release
  and rolls the Unreleased block into a versioned section.
- The existing 159 ADR rows are split into per-slug fragment files. The
  frozen `_order.txt` records the existing row order verbatim. New PRs
  append one slug to `_order.txt` (one-line conflict at worst, trivial
  concatenation merge).

The Doc-Substance Gate (ADR-0167) recognises a new
`changelog.d/<section>/<row>.md` as a CHANGELOG entry. The PR template
(`.github/PULL_REQUEST_TEMPLATE.md`) instructs contributors to add
fragment files instead of editing the consolidated outputs directly.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **In-tree shell scripts (chosen)** | Zero new deps; matches the fork's existing in-tree-script house style (`scripts/ci/*.sh`, `scripts/docs/*.sh`); ~90 LOC total; trivially auditable | Slight bash idiosyncrasy across runners (mitigated: `set -euo pipefail`, no GNU-isms beyond `find`/`awk`/`sort` already used elsewhere) | **Chosen** |
| `changie` upstream tool | Mature, `--kind` `--body` interactive UI, opinionated semantic-versioning awareness | New Go binary dependency for a problem that fits in 80 lines; fork avoids new tool installs unless they buy a load-bearing capability; release-please already owns the version-bump story | Rejected |
| `release-please` native fragments (`extra-files` + path globs) | No new code | release-please's fragment support is template-string-driven; mapping to per-section per-fragment markdown is awkward and would still require an in-tree concat step for the ADR-index half (release-please doesn't know about ADR README) — the unified shell pattern is simpler to maintain | Rejected (split tooling worse than dual-script symmetry) |
| Per-PR file under one `changelog.d/` flat dir, section in front-matter | Marginally simpler filenames | Loses the per-section sort-cheaply property; front-matter parsing adds awk complexity for no readability win | Rejected |
| Section-aware filename (`added__T7-39-foo.md`) in flat dir | Single dir to scan | Filename gets long; section-as-subdir is the Keep-a-Changelog idiom and it's how Sphinx/towncrier organise the same problem | Rejected |
| Manual concat at release time only | No CI infrastructure cost | Drift between fragments and CHANGELOG.md goes unnoticed for weeks; reviewers can't see the rendered changelog at PR time | Rejected (CI auto-regen is cheap and surfaces drift immediately) |

## Consequences

**Positive**

- New PRs add files instead of editing two consolidated 3500/250-line
  files → near-zero merge conflict surface for the changelog/ADR index
  pair.
- Fragment files are tiny and self-contained; reviewers see exactly what
  goes into the rendered changelog without reading the full consolidated
  file.
- `--check` lane catches drift between fragments and rendered output
  immediately; release-please runs `--write` at release-tag time.

**Negative**

- Two new conventions for contributors to learn (one fragment file per
  PR, plus one `_order.txt` line for ADRs). Mitigated by PR template
  carrying explicit instructions and by the Doc-Substance Gate
  recognising fragments.
- `_order.txt` is still a one-line-per-PR shared file → very small
  conflict surface remains, but resolution is mechanical
  (concatenate both lines).
- Legacy archive (`_pre_fragment_legacy.md`) freezes 3119 lines of past
  bullets in one file. Acceptable: those bullets are immutable history,
  and release-please will roll them into a versioned section at the next
  release tag.

**Neutral / follow-ups**

- The release-please workflow (`.github/workflows/release.yml`) gains a
  `--write` step before its CHANGELOG patch so the rendered Unreleased
  block is the input release-please consumes. (T7-39b — wire-up PR
  follow-up; ships once a release-please dry-run validates the
  end-to-end path. The scripts and migration land in this PR; the
  release-please integration step is a focused follow-up to keep this
  PR atomic.)
- A future PR may collapse the legacy archive at the next release cut
  by deleting `_pre_fragment_legacy.md` and letting release-please
  generate the versioned section purely from Conventional Commits.

## References

- `req` (paraphrased, per global rule on user-quote handling): every PR
  in flight this session fights merge conflicts in CHANGELOG.md and
  docs/adr/README.md; introduce fragment files so each PR adds a new
  file instead of touching the consolidated outputs.
- [ADR-0028](0028-adr-maintenance-rule.md) — ADR maintenance rule (the
  index row is the part this ADR moves to fragments).
- [ADR-0167](0167-doc-drift-enforcement.md) — Doc-Substance Gate
  recognising fragment files as CHANGELOG entries.
- [Research-0034](../research/0034-changelog-fragment-pattern.md) —
  cost-of-merge-conflict measurement + tool comparison.
- Keep-a-Changelog (<https://keepachangelog.com/>) — section ordering
  convention.
- towncrier (<https://towncrier.readthedocs.io/>) — Python-ecosystem
  precedent for fragment-file changelogs.

### Status update 2026-05-08

Operational drift between fragments and the rendered `CHANGELOG.md`
"Unreleased" block is reconciled by periodic drift-sweep PRs (in
addition to the per-PR `--check` CI lane). The drift-class table
(silent loss / orphan / duplicate) and the regenerate-vs-append-vs-
backfill resolution policy is documented in
[`docs/development/release.md`](../development/release.md#changelogmd-fragment-workflow-adr-0221).
This ADR's body remains frozen per ADR-0028; the policy doc is the
operational manual.
