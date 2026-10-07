<!-- markdownlint-disable MD013 MD060 -->
# ADR-2197: Generated changelog, ADR index and rebase notes are rendered when pull requests land

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `docs`, `release`, `process`

## Context

[ADR-0221](0221-changelog-adr-fragment-pattern.md) moved the changelog and the
ADR index to per-pull-request fragment files, so that two pull requests do not
edit the same line. It left two shared edits behind: a pull request still
committed the rendered `CHANGELOG.md` and `docs/adr/README.md` (the
`check-generated-docs` hook and the `Docs` job compared them with the
fragments), and it appended its slug to `docs/adr/_index_fragments/_order.txt`.
Three more outputs are shared the same way: the by-tag and title pages
generated from the ADR files, and `docs/rebase-notes.md`, to which every pull
request added an entry at the top.

All of them were union-merged (`.gitattributes` `merge=union`). That driver is
local to Git: GitHub does not apply it. After master moved, a pull request that
had edited any of them showed `CONFLICTING`, and GitHub starts no
`pull_request` workflow for a conflicting pull request. This was observed on
pull request 2390 (2026-10-07) over five rebases: every push cycle produced a new conflict
in `CHANGELOG.md`, `docs/adr/titles.md`, `docs/adr/by-tag/ci.md` or
`scripts/ci/source-adr-citations.json` before any check could run, with 200
hosted runs already queued.

The maintainer decided (Q-077, 2026-10-07) that the render happens at landing,
not in the pull request, and extended it to the by-tag and title pages (Q-080)
and the rebase notes (Q-081). The citation registry is a separate decision
(Q-082, its own ADR).

## Decision

A pull request adds **inputs only**: `changelog.d/<section>/*.md`,
`docs/adr/_index_fragments/<slug>.md`, `docs/rebase-notes.d/<slug>.md` and the
ADR and research files themselves. It does not touch the outputs:

| Output | Rendered from |
|---|---|
| `CHANGELOG.md` (Unreleased block) | `changelog.d/` |
| `docs/adr/README.md` | `docs/adr/_index_fragments/` |
| `docs/adr/by-tag/*`, `docs/adr/titles.md`, `docs/research/titles.md` | the ADR and research files |
| `docs/rebase-notes.md` (the fragment block at its top) | `docs/rebase-notes.d/` |

- **One command renders them**: `make docs-render`, a subset of
  `make docs-fragments-write` (which now calls it, then writes the outputs that
  stay in the pull request). The existing generators are the renderers; one is
  new, `scripts/docs/concat-rebase-notes.sh`, in the pattern of its siblings, and
  one helper, `scripts/docs/fragment-order.py`, is the single implementation of
  the ordering used by the ADR index and the rebase notes.
- **Who runs it**: the merge train, once per landing batch at the batch tip,
  committing `chore(docs): render generated changelog and ADR index` in the same
  push as the batch; and the release cut (`docs/development/release.md`).
- **Order without a manifest.** `_order.txt` is frozen as the list of the rows
  that existed at the migration; it is never edited again. Every other ADR index
  row, and every rebase note, is ordered by the commit that first added its
  fragment, read from history. Fast-forward landing makes that the landing order,
  which is what appending to `_order.txt` recorded, so the index shows the same
  order as before. A shallow clone fails loudly instead of guessing.
- **Rebase notes**: `docs/rebase-notes.md` keeps everything written before this
  decision in place. The renderer owns the block between two marker comments at
  the top and lists the fragments newest first, as the changelog renderer owns
  the Unreleased block.
- **PR gate**: `scripts/ci/deliverables-check.sh` (the `Deliverables Checklist`
  gate and the merge train's per-pull-request gate) refuses a diff that touches
  any output, and its CHANGELOG and rebase-note items now require a fragment
  file. The release pull request, whose workflow does not run that gate, still
  carries the render of the cut.
- **`make docs-fragments-check`** no longer compares the outputs above: it
  validates the fragments (changelog sections, ADR index rows, rebase-note
  headings) and the outputs that stay in the pull request. `make
  docs-render-check` compares them and fails on a stale render; the master push
  runs it in the `Docs` job (and the release pull request runs the changelog
  comparison). The train pushes the render with the batch, so master is never
  stale between batches; a push that skipped the render turns master red, which
  is the report.
- **`merge=union` is retired** for these files: no pull request edits them, so
  a conflict on one is a real one.

Outputs that stay in the pull request, with the reason (not decided here): the
`AGENTS.md` indexes (agents read them in the pull request), the exact-twin and
upstream-parity tables and the hardware-report and chart renders (user-facing
documentation whose inputs change in the same pull request and are read there).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the render in the pull request and resolve conflicts at rebase | No new machinery | GitHub shows the pull request conflicting after every master move, and no workflow starts for it; every rebase re-conflicts | The failure this ADR exists to remove |
| A bot commits the render to every pull request branch | The pull request keeps its render | Pushes to a contributor's branch; races with the author; one more writer of the same file | Two writers, same conflict |
| Render only at the release cut | Fewest renders | Master carries a stale changelog and index for days; the index is the entry point of the ADR site | Master must stay readable |
| Order the ADR index by ADR number | No history read | Changes what the index shows (132 numeric descents in the last 400 rows) | The index must not change |
| Order by a generated `_order.txt` the train appends to | No history read at render time | A second writer of a manifest, and the order of one batch is still a derivation | History already holds the order |
| Move every output of `docs-fragments-write` | One rule | The AGENTS indexes and the user-facing tables are read in the pull request | Not the same decision; left in place |

## Consequences

- **Positive**: a pull request touches no shared line of the changelog, the ADR
  index, the tag and title pages or the rebase notes, so GitHub reports it
  mergeable after master moves and its workflows run. `merge=union` is no longer
  load-bearing.
- **Negative**: the rendered files lag a landing by the train's render step; a
  reader of master between a batch's pushes sees the previous render only if the
  push skipped it (the master check reports that). A pull request in flight that
  still edits an output is refused until it drops the edit. The render reads full
  history, so a shallow clone cannot render.
- **Neutral / follow-ups**: the merge train runs `make docs-render` and commits
  the result (wired in `train.py`, not in this repository). The citation registry
  `scripts/ci/source-adr-citations.json` has the same conflict shape and is
  handled by its own ADR (Q-082).

## References

- `Q-077` (maintainer decision, 2026-10-07): render at release and once per
  landing batch; pull requests add inputs only.
- `Q-080`: the by-tag and title pages render at landing too.
- `Q-081`: the rebase notes become fragments under `docs/rebase-notes.d/`.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) (superseded in part: who
  renders and when), [ADR-0108](0108-deep-dive-deliverables-rule.md) (items e and
  f), [ADR-0913](0913-changelog-renderer-splice-contract.md),
  ADR-2169 (the CI tiers whose pull requests this unblocks).
