# `docs/adr/_index_fragments/` — per-ADR index-row fragments

The fork's `docs/adr/README.md` index table is **rendered**, not edited
directly. Each PR that adds an ADR drops one fragment file here:

```
docs/adr/_index_fragments/
  _header.md     verbatim README prelude (intro + Format + Conventions
                 + Why + Tag palette + the table-header row). Edit when
                 the prelude itself genuinely changes.
  _order.txt     frozen commit-merge order. One slug per line; new PRs
                 append their slug to the bottom.
  <NNNN-slug>.md one Markdown table row per ADR, named by the same
                 NNNN-kebab-case used for the ADR file itself. Slug-keyed
                 (not bare-NNNN) so renumbering events (e.g. the
                 2026-05-02 dedup sweep that moved ten duplicate-NNNN
                 files into 0242–0251) don't churn fragment filenames
                 — the slug stays stable across the remap.
```

## How to add a row

1. Land your ADR file as usual at `docs/adr/<NNNN-slug>.md`.
2. Create `docs/adr/_index_fragments/<NNNN-slug>.md` containing the
   single Markdown table row (the same line you would have appended to
   `README.md`).
3. Append `<NNNN-slug>` (no extension, no leading path) to
   `docs/adr/_index_fragments/_order.txt`.
4. Run `bash scripts/docs/concat-adr-index.sh --write` to regenerate
   `README.md` locally before pushing. CI runs `--check` and fails on
   drift.

## How fragments render

`scripts/docs/concat-adr-index.sh` emits `_header.md` first, then for
each line in `_order.txt` it concatenates the matching fragment file
(rows render in commit-merge order, matching the historical
`README.md`). Any fragment not yet listed in `_order.txt` is appended
at the bottom in lexical order.

## Why fragments

See [ADR-0221](../0221-changelog-adr-fragment-pattern.md) (the
fragment-file pattern itself). Short
version: PR-pairs editing the `README.md` index table directly fight a
merge conflict on the final-row context line. Fragment files are
per-path, so two PRs in flight don't collide.
