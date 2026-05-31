# `changelog.d/` — per-PR CHANGELOG fragment files

The fork's `CHANGELOG.md` Unreleased block is **rendered**, not edited
directly. Each PR adds one fragment file under one of the
Keep-a-Changelog sections:

```text
changelog.d/
  added/        new user-discoverable surface
  changed/      enhancement to an existing surface (includes perf — name files perf-<topic>.md)
  deprecated/   surface marked for removal
  removed/      surface deleted
  fixed/        bug fix with user-visible delta
  security/     security-affecting change
  _pre_fragment_legacy.md   verbatim archive of the pre-fragment Unreleased
                            block (do not edit; rolled into a versioned
                            section at the next release tag)
```

There is **no `perf/` or `performance/` directory** — the renderer
warns on any non-Keep-a-Changelog subdirectory and skips its
contents. Performance fragments live in `changed/` with a `perf-`
filename prefix so they sort contiguously inside the rendered
`### Changed` section. See [ADR-0892](../docs/adr/0892-conventional-commits-and-changelog-fragment-hygiene.md)
and [ADR-0913](../docs/adr/0913-changelog-renderer-splice-contract.md).

## How to add a fragment

1. Pick the section directory matching your change (Keep-a-Changelog).
2. Create one file `changelog.d/<section>/<task-id>-<topic>.md`. The
   filename is sorted lexicographically inside the section, so a task-id
   prefix (e.g. `T7-39-changelog-fragments.md`) gives implicit ordering.
3. Write a Markdown bullet (or a small block of bullets) — same shape as
   what you would have pasted into `CHANGELOG.md`.
4. Run `bash scripts/release/concat-changelog-fragments.sh --write` to
   regenerate `CHANGELOG.md` locally before pushing. CI runs `--check`
   and fails on drift.

## How fragments render

`scripts/release/concat-changelog-fragments.sh` emits the legacy
archive first (preserves migration content end-to-end), then for each
section it concatenates the `*.md` files in lexical order under one
`### Section` heading. Sections with no fragments are skipped. The
rendered body replaces the existing `## [Unreleased]` block in
`CHANGELOG.md`.

## Why fragments

See [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md).
Short version: every PR-pair editing `CHANGELOG.md` directly fights a
merge conflict on the section-header line. Fragment files are
per-path, so two PRs in flight don't collide.
