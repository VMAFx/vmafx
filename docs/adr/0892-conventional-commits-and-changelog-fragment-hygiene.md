# ADR-0892: Conventional-Commits coverage + Changelog-fragment section hygiene

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: process, release, changelog, ci, fork-local

## Context

A scheduled audit of the last 50 commits on `origin/master` and of the
`changelog.d/` fragment tree surfaced two release-please correctness
gaps that the existing pipeline silently absorbed:

1. **Three valid Conventional-Commits types had no release-please
   section configured for the root package** — `revert`, `security`,
   `style`. One commit on master (`d0b697c6` —
   `revert: drop continue-on-error on release-please …`) used `revert`,
   which falls through the configured-section list in
   `release-please-config.json` and lands under no heading in the
   generated CHANGELOG. The next time a security-flagged or style-only
   commit reaches the trunk it would hit the same drop.
2. **Thirty-two changelog fragments lived under invalid section
   directories** — `changelog.d/perf/` (27 files) and
   `changelog.d/performance/` (5 files). Keep-a-Changelog has no
   "Performance" section, and the renderer in
   `scripts/release/concat-changelog-fragments.sh` iterates a
   hard-coded section list (`added → changed → deprecated → removed →
   fixed → security` per ADR-0221). Both directories were therefore
   silently skipped at render time, so every performance entry written
   to `perf/` or `performance/` would never appear in the rendered
   `Unreleased` block. The contributors clearly believed `perf` was
   conventional — the Conventional-Commits spec lists `perf` as a
   recognised type, and `release-please-config.json` even has a
   `{ "type": "perf", "section": "Performance" }` row for the
   commit-message lane. But the fragment lane is governed by ADR-0221
   and the bash renderer, not by release-please, so the two pipelines
   disagreed.

The commit-side gap (gap 1) corrupts the rendered CHANGELOG by
dropping section assignment for any future `revert:` / `security:` /
`style:` commit. The fragment-side gap (gap 2) had already silently
lost ~32 performance bullets that contributors had written in good
faith — at release time those entries would not have appeared in any
generated `[Unreleased]` block.

## Decision

Two coordinated changes:

1. **Extend the root package's `changelog-sections` in
   `release-please-config.json`** to cover the three missing standard
   Conventional-Commits types: `revert → Reverts`, `security →
   Security`, `style → Style`. The `ai` package gets the same three
   entries (it inherits all root types in practice and the smallest
   release-please packages — `dev-llm`, `mcp-server/vmaf-mcp` — keep
   their minimal type sets, since their commit traffic is narrow and
   doesn't see these types in practice).
2. **Merge `changelog.d/perf/` and `changelog.d/performance/` into
   `changelog.d/changed/`** with a `perf-` filename prefix so the
   entries sort together inside the rendered `### Changed` section.
   The two source directories are removed entirely; the
   `changelog.d/README.md` is updated to call out that performance
   work belongs in `changed/perf-<topic>.md`. Each migrated fragment
   had its leading `Performance` / `perf(…)` heading stripped (the
   renderer adds `### Changed` itself), and any other in-body heading
   was demoted to a bold-prefixed bullet so it sits cleanly under the
   rendered section heading.

The rule going forward is that the `changelog.d/` section dirs are
**exactly** the six Keep-a-Changelog sections — `added`, `changed`,
`deprecated`, `removed`, `fixed`, `security`. Any other directory is
a contributor mistake and the renderer will not emit its contents.
The `perf-` filename prefix is the established convention for
performance work, and the same pattern can be applied to other
thematic groupings inside a Keep-a-Changelog section (e.g.
`build-`, `ci-`) without changing the renderer.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Move perf fragments to `changed/` + extend release-please types (chosen)** | Aligns the fragment tree with the documented ADR-0221 section list; restores 32 lost bullets to the rendered CHANGELOG; gives release-please a section for every type it will see on this branch | Touches 32 fragment files (mechanical rename + heading scrub) | **Chosen** |
| Teach the renderer a `performance/` section | Smaller diff (one bash array + the README) | Diverges from Keep-a-Changelog standard the rest of the docs reference; sets a precedent that any future ad-hoc directory ("research/", "ai/") can be added rather than mapped to a KaC section; ADR-0221 explicitly lists the six KaC sections — adding a seventh would need a supersedes-ADR | Rejected — ADR-0221's section list is the contract, not the renderer |
| Delete the orphan fragments outright | Trivially restores invariant | Throws away 32 contributor-written bullets that describe real merged work | Rejected — content is real history; loss is not acceptable |
| Leave commit-side gap (`revert`/`security`/`style`) open, document workaround | Zero risk | Next `revert:` / `security:` commit on master silently loses its CHANGELOG section assignment; the audit just-now found one such commit already in tree | Rejected — the cost to add three section rows is two lines per package |

## Consequences

- **Positive**: The rendered CHANGELOG is now self-consistent — every
  fragment lives in a directory the renderer iterates, and every
  Conventional-Commits type used on this branch maps to an explicit
  release-please section. Future drift is bounded by the README's
  explicit enumeration of the six allowed directories.
- **Positive**: Performance entries inside `### Changed` are
  discoverable because the `perf-` filename prefix sorts them
  contiguously; readers see a perf-cluster inside the Changed
  section instead of a scattered mix.
- **Negative**: 32 fragment files were renamed and their headings
  rewritten. Anyone holding a branch that touches one of the moved
  files will hit a rename-detection conflict on rebase. Mitigation:
  rebase early; `git diff --find-renames` resolves cleanly.
- **Neutral / follow-ups**:
  - The `--check` lane is already in place via the renderer but is
    not yet a required CI status; tightening that to a required
    check is a separate (small) follow-up.
  - `_pre_fragment_legacy.md` still encodes a 3500-line frozen
    archive; collapsing that block at the next release tag is
    tracked in ADR-0221's status-update note.

## References

- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — fragment-file
  pattern, defines the six Keep-a-Changelog section list.
- [`scripts/release/concat-changelog-fragments.sh`](../../scripts/release/concat-changelog-fragments.sh)
  — the renderer (hard-coded `SECTIONS=(added changed deprecated
  removed fixed security)` array).
- [`release-please-config.json`](../../release-please-config.json) —
  per-type section mapping for the commit-message lane.
- [`docs/research/0892-conventional-commits-audit-2026-05-30.md`](../research/0892-conventional-commits-audit-2026-05-30.md)
  — full audit log (commit-by-commit table + fragment inventory).
- [Keep-a-Changelog](https://keepachangelog.com/) — defines the six
  section types as the standard.
- Source: scheduled audit run on 2026-05-30 against
  `master@83698bd5b2`; surfaced one in-tree `revert:` commit + 32
  fragment files in undocumented directories.
