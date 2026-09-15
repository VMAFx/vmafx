<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1233: GitHub generates the release body; CHANGELOG.md keeps an index

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: ci, docs, build

## Context

Two separate surfaces describe a release, and both had problems.

**The release body.** release-please generated it from commit subjects. It was
in fact *complete* — 160 PRs merged since the release floor, 160 cited — but it
read as a flat list of commit subjects with links pointing at `/issues/N`, no
contributor attribution and no compare link.

**`CHANGELOG.md`.** It is rendered from `changelog.d/` fragments, and the
Unreleased block had grown to **27,502 lines from 1,661 fragments** —
the fork's entire pre-1.0 history, since the fork has never cut a release. The
rollover script versions that block verbatim, so `1.0.0` would have produced a
27,000-line section: unreadable, and slow enough to render that GitHub
truncates it.

There was also a precondition nobody had noticed. GitHub composes release notes
by grouping merged PRs **by label**, and this repository labelled almost
nothing: **32 of the last 40 merged PRs carried no label at all**, the only
label in regular use being `dependencies` from Renovate. Switching to
GitHub-native notes without fixing that would have filed ~80% of the work under
"Other changes" — strictly worse than the commit-subject list it replaced.

## Decision

Set `changelog-notes-type: "github"` so GitHub composes the release body from
merged pull requests, with sections defined by `.github/release.yml`.

Make that work by deriving labels rather than requiring people to apply them.
`.github/workflows/pr-type-label.yml` maps each PR's Conventional-Commit prefix
to a `type:*` label — the prefix is already mandatory here, enforced by the
`commit-msg` hook, so the type is always available and nobody has to remember
anything. The 160 PRs already merged were backfilled from their commit
subjects, so the first release categorises correctly.

For `CHANGELOG.md`, `rollover-changelog-fragments.sh` gains `--archive-over N`
(default 400). Above that, the rendered body moves to
`docs/changelog-archive/X.Y.Z.md` and the version section keeps a per-section
index linking to it. Nothing is discarded, and the receipt still records the
sha256 of the full rendered body. For `1.0.0` this turns a 27,502-line section
into a 15-line index over 2,345 entries (Fixed 1227, Changed 559, Added 490,
Security 39, Removed 30).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **GitHub-native notes + derived labels** (chosen) | Per-PR attribution, contributors, compare link; grouping is data-driven; labels come free from a prefix that is already mandatory | Depends on a labeler workflow; a non-conventional title yields no label | Chosen — the labeler removes the only real objection |
| Keep commit-subject notes, polish them | No new machinery; already complete | Still a flat list; no contributors or compare link; `/issues/N` links are not configurable | Rejected — the ceiling is low |
| GitHub-native notes without a labeler | One-line config change | ~80% of PRs fall into "Other changes"; worse than the status quo | Rejected — measured, not assumed: 32 of 40 recent PRs were unlabelled |
| Require contributors to label PRs by hand | No workflow to maintain | The evidence says this does not happen; it is also redundant with a mandatory commit prefix | Rejected |
| Ship the 27k-line CHANGELOG section as-is | Zero work; nothing moves | `CHANGELOG.md` becomes unreadable and GitHub truncates it | Rejected |
| Hand-compress the 1,661 fragments | Best-looking result | A large manual pass that discards per-PR granularity | Rejected — the archive keeps the detail at no cost |

## Consequences

- **Positive**: release bodies gain per-PR attribution, a New Contributors
  section and a Full Changelog compare link, and group by an explicit,
  reviewable category list.
- **Positive**: `CHANGELOG.md` stays readable across releases without losing
  detail; the archive is a tracked file.
- **Positive**: the repository is labelled for the first time, which also makes
  PR search and filtering useful.
- **Negative**: a PR whose title is not Conventional-Commit gets no `type:`
  label and lands in "Other changes". The workflow emits a notice rather than
  failing, because the `commit-msg` hook already enforces the format on the
  commit; a mislabelled PR is a cosmetic release-note problem, not a reason to
  block a merge.
- **Negative**: `pull_request_target` is required to label PRs from forks. The
  workflow checks out no PR code and only calls the labels API, so untrusted
  code never executes.
- **Neutral / follow-ups**: `changelog-sections` in `release-please-config.json`
  is retained; it still shapes `CHANGELOG.md`, which is rendered from fragments
  and is a different surface from the release body.
