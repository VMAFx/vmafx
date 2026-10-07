## Rendered docs: pull requests carry fragments only (2026-10-07)

`ci/render-at-release`, [ADR-2197](adr/2197-render-generated-docs-at-landing.md). Fork-only tooling. `docs/rebase-notes.md` has a
fragment block at its top (between two marker comments) rendered from `docs/rebase-notes.d/`; the entries below the block are the
history and stay as they are. `CHANGELOG.md`, `docs/adr/README.md`, `docs/adr/by-tag/`, `docs/adr/titles.md` and
`docs/research/titles.md` are outputs of `make docs-render`: on a conflict in one, take master's side and re-render, and never
add them back to a branch (`deliverables-check.sh` refuses them). `_order.txt` is frozen; a rebased branch drops any line it added.
`.gitattributes` no longer lists `merge=union` for these files.
