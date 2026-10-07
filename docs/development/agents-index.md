<!-- markdownlint-disable MD013 -->
# Agents index and topic pages

Every directory with rules an agent must respect has an `AGENTS.md`. A large
one is split: the `AGENTS.md` is a short index, and the rules live in one page
per topic under `AGENTS.d/` next to it. An agent reads the index, matches the
paths it is about to touch against the table in it, and reads only the pages
that match. [ADR-1454](../adr/1454-agents-index-and-topic-pages.md) records the
decision and the measurements; `scripts/ci/` was the first directory in this
shape; `find . -name AGENTS.d` lists every directory that has one today.

A directory whose `AGENTS.md` has no `AGENTS.d/` next to it is still a single
hand-written file and is edited as before.

## Layout

```text
scripts/ci/
  AGENTS.md              generated index, never edited by hand
  AGENTS.d/
    _index.md            title and the rules for every file of scripts/ci/
    tidy-ratchet.md      one topic
    parity-gate.md       one topic
```

## Add an invariant

1. Open `AGENTS.md` in the directory you changed and find the row whose
   `Touching` column matches your files.
2. If a row matches, add your text to that page. The index does not change,
   so your pull request does not conflict with another one that adds to a
   different page.
3. If no row matches, add a page (next section).
4. If the rule applies to every file of the directory, put it in
   `AGENTS.d/_index.md` instead of a page.
5. Run `make docs-fragments-write` and commit the page together with any
   change to `AGENTS.md`.

## Add a page

A page is `AGENTS.d/<slug>.md`. The slug is lower-case words joined by `-` and
names the topic: a tool, a feature or a contract. Do not name a page after a
date or a pull request.

```markdown
---
paths:
  - scripts/ci/tidy-ratchet.py
  - scripts/ci/tidy-baseline-*.json
invariant: Baselines only via `--write` on the lane's own toolchain; counts only decrease.
---
# tidy-ratchet.py invariants (ADR-1142)

The rules, written as in any other `AGENTS.md`.
```

- `paths:` lists the files the page governs, as globs relative to the
  repository root (`*` within one path segment, `**` across segments). Include
  the tests and the workflow or configuration files that must change together
  with the code. Every glob must match at least one file.
- `invariant:` is one line, at most 120 characters: the rule an agent must not
  break even if it never opens the page.
- The body starts with a level-one heading (`# Title`). Links are relative to
  the page, which is one directory deeper than the index:
  `../../../docs/adr/...` from `scripts/ci/AGENTS.d/`.
- The body, `_index.md` and the `invariant:` line are agent text in the
  internal register. Since the praetor pin `afb739ed81f3`
  ([ADR-2321](../adr/2321-praetor-pin-afb739ed.md)), `praetorctl
  compile-context --verify` and `praetorctl audit` lint every tracked nested
  `AGENTS.md`, and the index is rendered from these three. Check a page with
  `praetorctl caveman check --kind=context <page>` and the index after
  `make docs-fragments-write`. The usual finding is article density above
  2.0 per 100 prose words; drop the articles and split sentences longer than
  30 prose words at a `;` or `:`.

## Regenerate and check

```bash
make docs-fragments-write   # render every AGENTS.md that has an AGENTS.d/
make docs-fragments-check   # fail when one is stale or a page is invalid
python3 scripts/docs/agents_index.py --check scripts/ci   # one directory
```

The `check-generated-docs` pre-commit hook runs the check whenever an
`AGENTS.md` or a file under an `AGENTS.d/` is staged. When a rebase conflicts
in a generated `AGENTS.md`, take master's side and run
`make docs-fragments-write`; never resolve the generated file by hand.

`scripts/docs/agents_index.py --check` exits 1 when an `AGENTS.md` differs from
its rendering. That covers an index that is stale against its pages and one
that was edited or appended to by hand, which is how the single files grew.
For a hand edit the message quotes the lines the sources do not produce and
names the directory they belong in:

```text
agents index: scripts/ci/AGENTS.md is generated and holds 2 line(s) its sources do not produce:
    ## New gate invariant
    `new-gate.sh` must exit 2 on a missing body.
  Never edit or append to scripts/ci/AGENTS.md. An invariant goes into a page under scripts/ci/AGENTS.d/
  (scripts/ci/AGENTS.d/_index.md only when it binds every file of the directory); then run
  `make docs-fragments-write`, which overwrites the generated file.
```

Move the text into a page first: `make docs-fragments-write` discards whatever
was typed into the generated file.

The script exits 65 for a source it rejects. It rejects:

| Finding | Limit or rule |
| --- | --- |
| Index too large | 16,000 bytes: shorten `invariant:` lines or merge pages |
| Page too large | 12,000 bytes: split the page by topic |
| `invariant:` too long | 120 characters |
| Too many globs | 24 per page |
| Glob matches nothing | the file was renamed or removed: update the page |
| Front matter | exactly `paths:` (a list) and `invariant:`; no other key |
| File name | lower-case words joined by `-`, ending in `.md` |
| Body | must start with a level-one heading (`# Title`) |
| Anything else in `AGENTS.d/` | only Markdown pages and `_index.md` |

## What the index contains

`AGENTS.md` is assembled in this order: a banner that says it is generated;
the title from `_index.md`; a paragraph that tells an agent how to use the
table; the rest of `_index.md`; and the table, one row per page sorted by
slug. A row shows the page's globs (relative to the directory, with a leading
`/` for paths outside it), a link to the page, and its `invariant:` line.

## Migrate an existing `AGENTS.md`

A migration moves text and changes nothing else, so that a reviewer can read
the pull request as a move. Pages add only structure: front matter, a title,
repeated headings and repeated table headers. Relative link targets gain one
`../` because a page is one directory deeper.

`scripts/docs/agents_migration_check.py` proves that nothing was lost:

```bash
python3 scripts/docs/agents_migration_check.py --old-ref origin/master scripts/ci
```

It compares the `AGENTS.md` at the given revision with `AGENTS.d/_index.md`
and the pages, and exits 1 unless all of the following hold:

- every paragraph, top-level list item, table row and code block of the old
  file is in the new text exactly as often as before, byte for byte, with
  link targets compared after resolving them to the repository root;
- every heading text of the old file is still present;
- every code span, path, ADR, pull-request, issue or ledger id, flag,
  identifier, number and link target of the old file is still present;
- every table row has a header above it, and the generated index is fresh.

Its output for the pilot is quoted in the pull request that introduced the
structure. `scripts/docs/tests/test_agents_index.py` holds the tests of both
scripts and runs as the `test-agents-index` pre-commit hook.

The files that are still to be migrated are listed under
`T-AGENTS-INDEX-MIGRATION-2026-10-02` in [`docs/state.md`](../state.md).
