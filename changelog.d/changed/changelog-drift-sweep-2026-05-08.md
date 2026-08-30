- **CHANGELOG.md drift sweep — 2026-05-08
  ([ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md)).**
  Reconciled accumulated skew between `changelog.d/<section>/*.md`
  fragments and the rendered `## [Unreleased]` block of
  [`CHANGELOG.md`](../CHANGELOG.md). Thirteen fragments had landed in
  in-flight PRs without `--write` being run; one entry (vmaf-tune
  `--score-backend=vulkan`) had a verbose inline form that drifted from
  its canonical fragment; one entry (FastDVDnet `smoke: false` flip)
  was duplicated twice in the rendered block; and the `### Changed`
  header itself was duplicated. Regenerated via
  `scripts/release/concat-changelog-fragments.sh --write` after manual
  inspection of every removal — no genuine orphans were found. Also
  documents the fragment-vs-rendered drift policy + drift-class table
  in [`docs/development/release.md`](../docs/development/release.md).
  Companion to PR #476 on the ADR-index side; both PRs touch the
  fragment-pattern ecosystem ADR-0221 establishes.
