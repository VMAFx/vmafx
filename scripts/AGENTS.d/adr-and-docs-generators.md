---
paths:
  - scripts/docs/*adr*
  - scripts/docs/agents_*.py
  - scripts/adr/**
  - mkdocs.yml
invariant: Generated ADR index, tag pages, AGENTS.md never hand-edited; next-free lock cwd-safe.
---
<!-- markdownlint-disable MD013 MD038 -->
# ADR and AGENTS.md generators (`scripts/docs/`, `scripts/adr/`)

## Rebase-sensitive invariants

### `docs/concat-adr-index.sh` is the source of truth for `docs/adr/README.md`

Per [ADR-0221](../../docs/adr/0221-changelog-adr-fragment-pattern.md),
`docs/adr/README.md` **rendered** from per-ADR fragments under
`docs/adr/_index_fragments/*.md`:

- `_header.md` = verbatim README prelude (everything before
  `## Index`).
- One Markdown row per ADR, named by ADR's full slug
  (`NNNN-kebab-case.md`). Slug-keyed for historical reasons:
  2026-05-02 dedup sweep renumbered duplicate-NNNN ADRs; slug
  filenames remain stable across that remap.
- Rows render oldest-first by ADR ID.

**On rebase**: do **not** edit `docs/adr/README.md` by hand. Add
or edit fragment file, run with `--write`. Renaming script
or changing slug-keyed naming breaks every ADR-ID remap
downstream.

### The ADR navigation in `mkdocs.yml` is three static lines (ADR-1510)

Per [ADR-1510](../../docs/adr/1510-adr-nav-collapse-behind-index.md), the
`ADRs` entry of `mkdocs.yml` `nav:` lists `adr/README.md`,
`adr/0000-template.md` and `adr/by-tag/index.md` and nothing else; the
per-hundred generator of ADR-0937 (`docs/generate-adr-nav.sh`) is gone.
Readers reach an ADR through the index, a tag page, a link or search.

**On rebase**: do not bring back `generate-adr-nav.sh`, its `--check` /
`--write` lines in `make docs-fragments-check` / `docs-fragments-write`, or an
`ADR-NAV-GENERATED` block; an ADR merge does not edit `mkdocs.yml`.
`docs/tests/test_generators.py::test_adr_navigation_is_collapsed` holds the
three entries. ADR coverage comes from the index check
(`docs/check-adr-index.py`) and the tag pages.

### `docs/generate-adr-by-tag.sh` owns the `docs/adr/by-tag/` tree

Script scans every ADR's `Tags:` front-matter field, rewrites
`docs/adr/by-tag/<tag>.md` (one Markdown table per tag) plus
`docs/adr/by-tag/index.md`. Hand-edits inside `by-tag/` lost on
next `--write` run. Accepts both `- **Tags**: …` (modern bullet)
and `Tags: …` (legacy bare) forms.

**On rebase**: do not hand-edit `docs/adr/by-tag/*.md`. To add or
remove tag, edit ADR's `Tags:` line, re-run script with
`--write`. Tag values containing whitespace or angle-bracket
placeholders (template examples) filtered out — keep filter
in place when adapting regex. Validate output basenames before writing,
deduplicate equivalent tags within one ADR, preserve generator-owned
lint header. Title escaping and meaningful code-span spaces = renderer
responsibilities; do not rewrite accepted ADRs to satisfy generated lint.
`docs/check-adr-index.py` also verifies fragment/source coverage and mutable
fragment ADR references before concatenation.

### `docs/agents_index.py` owns every `AGENTS.md` next to an `AGENTS.d/` (ADR-1454)

`<dir>/AGENTS.md` = rendered from `<dir>/AGENTS.d/_index.md` (title + rules for
every file) and `<dir>/AGENTS.d/<slug>.md` pages (front matter `paths:` list +
`invariant:`). Never hand-edit the index. New invariant = edit or add a page,
then `make docs-fragments-write`; `make docs-fragments-check` (hook
`check-generated-docs`) fails on a stale index and on one edited or appended
to by hand: `stale_message()` quotes the foreign lines and names the
`AGENTS.d/` they belong in. Keep that: appending to the index = how the old
files grew. Budgets in the script: index 16000 bytes, page 12000,
`invariant:` 120 characters, 24 globs; a glob matching no file fails. Raising
a budget = new ADR superseding that part of ADR-1454.

**On rebase**: conflict in a generated `AGENTS.md` -> master's side, then
`make docs-fragments-write`. Conflict in a page -> resolve by hand like any
source. `docs/agents_migration_check.py` = one-off proof for a migration PR
(old file vs pages: units, headings, tokens); `agents_index.py` imports nothing
from it, it imports `map_links` / `render` from the generator. Both tested in
`docs/tests/test_agents_index.py` (hook `test-agents-index`). Guide:
[agents index](../../docs/development/agents-index.md).

### `adr/next-free.sh --claim` lock + claim file independent of cwd

- Lock name = resolved absolute common git dir
  (`cd "$(git rev-parse --git-common-dir)" && pwd -P`). Relative `.git`
  vs `../.git` gave root and `docs/` callers different locks -> duplicate
  numbers (#2769 CI).
- Lock holds owner token; release only own lock.
- Exclusive claim file `.git/adr-claims/NNNN` (`noclobber`) decides number;
  taken -> N+1, max 100 tries.
- Test cleanups (`adr/test-next-free.sh`,
  `adr/tests/test-next-free-remote-aware.sh`) compute same name.
- Lock = owner file, created `noclobber` (O_EXCL) in lock dir; dir without
  owner file = free. Owner pid dead -> under `<lock>.takeover` mutex: pid
  re-checked, owner line re-compared, file removed with WARNING. Live holder
  never taken over. No grace takeover (raced fresh mkdir, soak 2). Exit trap
  set before mkdir. Wait = `VMAF_ADR_LOCK_WAIT_S`
  (default 60, was fixed 10) (#2784).
- Guards: `adr/tests/test-next-free-concurrent.sh`,
  `adr/tests/test-next-free-stale-lock.sh`.
