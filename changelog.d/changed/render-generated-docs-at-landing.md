- **A pull request no longer carries the rendered changelog, ADR index or rebase
  notes ([ADR-2197](docs/adr/2197-render-generated-docs-at-landing.md)).** It
  adds fragments: `changelog.d/<section>/*.md`,
  `docs/adr/_index_fragments/<slug>.md` and the new
  `docs/rebase-notes.d/<slug>.md`. `CHANGELOG.md`, `docs/adr/README.md`, the
  ADR by-tag and title pages and `docs/rebase-notes.md` are written by
  `make docs-render` when pull requests land (the merge train per batch, the
  release cut); `scripts/ci/deliverables-check.sh` refuses a pull request that
  edits one. `docs/adr/_index_fragments/_order.txt` is frozen: later rows follow
  in the order they landed. GitHub no longer reports such a pull request as
  conflicting after master moves.
