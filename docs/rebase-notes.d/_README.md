# `docs/rebase-notes.d/`: one rebase note per pull request

A pull request that has a rebase-sensitive effect adds **one file** here,
`docs/rebase-notes.d/<slug>.md`, and does not edit `docs/rebase-notes.md`.
The file starts with its heading and holds the note:

```markdown
## <what changed> (YYYY-MM-DD)

`<branch>`, [ADR-NNNN](adr/NNNN-slug.md). What an upstream sync or a rebase
must keep, and where the invariant is guarded.
```

`make docs-render` writes every file here into the fragment block at the top of
`docs/rebase-notes.md`, newest first (the order the files landed on the
branch). The merge train renders once per landing batch and the release cut
renders too, so the file you add is in `docs/rebase-notes.md` after it lands. A
pull request that edits `docs/rebase-notes.md` itself is refused by
`scripts/ci/deliverables-check.sh`
([ADR-2197](../adr/2197-render-generated-docs-at-landing.md)). The entries
written before the notes moved to fragments stay in `docs/rebase-notes.md`
below the block, as they were.

A file whose name starts with `_` (this one) is not rendered. Use
`no rebase impact: REASON` in the pull request body when there is nothing to
record.
