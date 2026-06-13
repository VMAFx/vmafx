<!-- markdownlint-disable MD018 -->
# Research-0034: CHANGELOG + ADR-index fragment-file pattern

- **Date**: 2026-04-29
- **Author**: Lusoris + Claude (Opus 4.7 1M-ctx)
- **Tracks**: [ADR-0221](../adr/0221-changelog-adr-fragment-pattern.md)
- **Status**: Decision

## Problem

During the 2026-04-28 → 2026-04-29 sprint, every fork-local PR under
review fought merge conflicts in two consolidated files:

- `CHANGELOG.md` (3751 lines; 3119 in the active `Unreleased` block).
- `docs/adr/README.md` (252 lines; 159 ADR rows in the index table).

Each PR adds one bullet to `Unreleased` and one row to the index. When
two PRs branch off the same merge base (the common case during sprint
work), git's three-way merge cannot resolve "PR-A adds row at end" +
"PR-B adds row at end" because both touch the same final-row context
line.

## Cost measurement

Sample of the 2026-04-28 → 2026-04-29 PR cohort (#190, #193, #194, #195,
#202, #181):

- Median rebase passes per PR before merge: **2** (range 1–4).
- Median time per rebase pass (manual conflict resolution + lint
  re-run + force-push + CI re-round): **8 minutes**.
- Per-PR overhead from the changelog/ADR conflict pair: **≈16 minutes**.
- 6 PRs × 16 min = **96 minutes** of pure bookkeeping cost in 48 hours.

The actual code review on each ran in seconds; the conflicts were
purely positional.

## Tool / pattern survey

### 1. In-tree shell-script fragment concat (chosen)

- **Approach**: each PR adds a new file under `changelog.d/<section>/`
  (Keep-a-Changelog sections) and `docs/adr/_index_fragments/`. Two
  shell scripts (`scripts/release/concat-changelog-fragments.sh`,
  `scripts/docs/concat-adr-index.sh`) render the consolidated outputs.
  `--check` is the CI gate; `--write` is the release-please / local
  pre-push call.
- **Pros**: zero new dependencies; matches existing in-tree-script
  house style (`scripts/ci/*.sh` already drive CI gates); ~90 LOC
  total; trivial to audit; works in any bash 4+ environment the fork
  already supports.
- **Cons**: Bash idiosyncrasy. Mitigated by `set -euo pipefail` and
  staying within the GNU-coreutils subset already used by other in-tree
  scripts.

### 2. `changie`

- **Approach**: Go binary, opinionated `changie new` interactive
  prompt, configurable section taxonomy.
- **Pros**: mature, well-documented; semantic-versioning aware.
- **Cons**: new Go binary dependency for a problem that fits in 80
  lines of bash; the fork minimises tool-install footprint unless a
  tool buys a load-bearing capability. `release-please` already owns
  version bumping.

### 3. `towncrier` (Python ecosystem precedent)

- **Approach**: per-PR fragment files keyed by issue/PR ID, rendered
  via Jinja templates.
- **Pros**: well-known to Python-side contributors; rich template
  system.
- **Cons**: another Python dependency in the dev environment;
  Jinja-template overhead is more machinery than a bash concatenator
  needs to do; doesn't help with the ADR-index half (towncrier is
  changelog-specific).

### 4. `release-please` native fragments (`extra-files` + path globs)

- **Approach**: lean entirely on release-please's existing `extra-files`
  hook to discover and inline fragments at release time.
- **Pros**: no new in-tree code.
- **Cons**: release-please's fragment support is template-string-driven
  and section-tagging via PR labels. Mapping per-section markdown
  fragments to release-please's expected shape is awkward, and
  release-please doesn't know about `docs/adr/README.md` at all — the
  ADR index would still need its own concat step. Splitting the
  changelog (release-please) and ADR (in-tree script) tooling is worse
  than running both through one symmetric shell pattern.

### 5. Per-PR file under flat `changelog.d/` with section in front-matter

- **Pros**: simpler filenames.
- **Cons**: loses the per-section directory invariant; front-matter
  parsing adds awk complexity for no readability win.

### 6. Manual concat at release time only

- **Pros**: no CI infrastructure cost.
- **Cons**: drift between fragments and `CHANGELOG.md` goes unnoticed
  for weeks; reviewers can't see the rendered changelog at PR time.
  CI auto-regen via `--check` is cheap and surfaces drift immediately.

## Decision

**Option 1 (in-tree shell-script fragment concat)** is chosen for both
surfaces. It matches the fork's existing house style, requires zero new
dependencies, and ships in a single ~90-line atomic PR.

Migration is content-preserving: the existing `Unreleased` body is
archived verbatim under `changelog.d/_pre_fragment_legacy.md`; the 159
ADR rows are split per-slug with a frozen `_order.txt` manifest that
preserves the existing commit-merge order.

## Open questions / follow-ups

- **release-please integration** (T7-39b): the `.github/workflows/release.yml`
  workflow needs a `--write` step before release-please patches
  `CHANGELOG.md`, so the rendered Unreleased block is the input
  release-please consumes. Ships once a dry-run validates the
  end-to-end path. Out of scope for this PR.
- **Legacy archive collapse**: at the next release cut, the
  `_pre_fragment_legacy.md` content rolls into a versioned section and
  the archive can be deleted. Optional clean-up follow-up.
- **`_order.txt` line-conflict surface**: still one shared file, still
  one line per PR. Trivial to resolve (concatenate both lines), but
  not zero. A future migration could replace `_order.txt` with
  per-fragment ordering metadata if the trivial-resolution cost ever
  becomes load-bearing.
