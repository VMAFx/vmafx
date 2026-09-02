- **CHANGELOG + ADR-index fragment files (T7-39 / ADR-0221)** — every PR
  in flight before this change fought merge conflicts in
  [`CHANGELOG.md`](CHANGELOG.md) and
  [`docs/adr/README.md`](docs/adr/README.md) (each PR adds a row, every
  other PR's row collides). PRs now drop a single fragment file under
  `changelog.d/<section>/<topic>.md` (Keep-a-Changelog sections: `added`,
  `changed`, `deprecated`, `removed`, `fixed`, `security`) and one row
  fragment under `docs/adr/_index_fragments/NNNN-slug.md`. Two new in-tree
  shell scripts —
  [`scripts/release/concat-changelog-fragments.sh`](scripts/release/concat-changelog-fragments.sh)
  and [`scripts/docs/concat-adr-index.sh`](scripts/docs/concat-adr-index.sh)
  — render `CHANGELOG.md`'s Unreleased block and `docs/adr/README.md`
  from the fragment trees; both ship `--check` (CI) and `--write`
  (release-please / local) modes. Migration is content-preserving: the
  existing 3119-line Unreleased body is archived verbatim under
  `changelog.d/_pre_fragment_legacy.md`, and 159 ADR rows are split into
  per-slug fragment files driven by a frozen `_order.txt` manifest that
  preserves the existing commit-merge order. New PRs append one fragment
  file (and one line to `_order.txt`) instead of editing the consolidated
  files. Doc-Substance Gate (ADR-0167) recognises a new
  `changelog.d/<section>/<row>.md` as a CHANGELOG entry. See
  [ADR-0221](docs/adr/0221-changelog-adr-fragment-pattern.md) +
  [`docs/research/0034-changelog-fragment-pattern.md`](docs/research/0034-changelog-fragment-pattern.md).
