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
| Load path too large | 32,768 bytes across every `AGENTS.md` from the repository root to the directory: move text into `AGENTS.d/` pages, parents first |
| Index too large | 16,000 bytes: shorten `invariant:` lines or merge pages |
| Page too large | 12,000 bytes: split the page by topic |
| `invariant:` too long | 120 characters |
| Too many globs | 24 per page |
| Glob matches nothing | the file was renamed or removed: update the page |
| Front matter | `paths:` (a list) and `invariant:`, plus optional `area:`; no other key |
| File name | lower-case words joined by `-`, ending in `.md` |
| Body | must start with a level-one heading (`# Title`) |
| Area | `area:` names an `_area-<slug>.md`; every area has a page; summary 1 to 400 characters |
| Anything else in `AGENTS.d/` | only Markdown pages, `_index.md` and `_area-<slug>.md` |

## Areas

When the table of one directory would pass the 16,000-byte index budget, group
its pages into areas (decision Q-115; `scripts/ci/` has four: gates, release,
tidy, tests). Each area has its own generated sub-index and its own budget.

1. Add `AGENTS.d/_area-<slug>.md`: a `# Title` line, then one summary
   paragraph (at most 400 characters) that says which files and topics the
   area covers. An agent routes by this summary.
2. Add `area: <slug>` to the front matter of every page. Once a directory has
   an area file, every page must name one, and every area needs a page.
3. Run `make docs-fragments-write`. `AGENTS.md` then lists the areas, and
   `AGENTS-<slug>.md` next to it holds the table of that area's pages, in the
   same form as the table of an undivided index.

An agent reads `AGENTS.md`, picks the area whose summary covers the paths it
will touch (both areas when unsure), matches the paths in that sub-index and
reads the matching pages. Page paths, links and the `invariant:` rule do not
change. `_area-<slug>.md` is not a page: it has no front matter and no row.

## Load-path budget

Codex reads one `AGENTS.md` per directory from the repository root down to the
working directory and truncates the deepest file once the byte sum passes
`project_doc_max_bytes` = 32,768 bytes (`codex-rs/core/src/agents_md.rs`;
[ADR-3540](../adr/3540-agents-chain-budget.md)). Codex, Gemini, Antigravity,
opencode, Claude Code and editors each bound always-loaded instructions, so
every load path (the sum of every tracked `AGENTS.md` from the repository root
to a directory, inclusive) must fit within the cap.

- **32,768 bytes enforced (`CHAIN_MAX_BYTES`)**: `scripts/docs/agents_index.py`
  (`make docs-fragments-check`) refuses any load path above 32,768 bytes with
  exit code 65.
- **30,720 bytes working target**: keep new and migrated load paths at or below
  30,720 bytes (30 KiB) so later topic rows have 2,048 bytes of headroom before
  tripping the gate.
- **Parents first**: shrink shared parent directories (`AGENTS.md`,
  `scripts/AGENTS.md`, `cmd/AGENTS.md`, `core/AGENTS.md`) before touching
  leaves. Every byte in a parent is paid by every directory below it; once a
  parent becomes a short `_index.md` head plus a rendered table or areas,
  smaller leaves stay under 30,720 bytes untouched, and only leaves whose path
  still passes 30,720 bytes migrate into `AGENTS.d/`.

When a chain exceeds the cap, `--check` prints the total, the cap and each file
contributing to the sum:

```text
agents index: scripts/release/AGENTS.md: load path 55484 bytes, chain budget 32768 (AGENTS.md 11158 + scripts/AGENTS.md 25444 + scripts/release/AGENTS.md 18882)
```

Read the parenthetic chain from left to right: split the largest unmigrated
parent first (`python3 scripts/docs/agents_index.py --write <dir>`), then split
the leaf if the chain still passes 30,720 bytes.

Measure every tracked load path in the tree with:

```bash
git ls-files -co --exclude-standard | grep -E '(^|/)AGENTS\.md$' | python3 -c 'import sys,os
fs=[l.strip() for l in sys.stdin]; sz={f:os.path.getsize(f) for f in fs}
for f in sorted(fs):
    d=os.path.dirname(f); parts=d.split("/") if d else []
    chain=["AGENTS.md"]+["/".join(parts[:i])+"/AGENTS.md" for i in range(1,len(parts)+1)]
    chain=[c for c in chain if c in sz]; print(sum(sz[c] for c in chain), d or ".", len(chain))' | sort -rn
```

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
