<!-- markdownlint-disable MD013 -->
# ADR Authoring Workflow

This page describes how to create a new Architectural Decision Record (ADR) in this
fork without colliding with a number already claimed by another agent or branch.

## Quick start

```bash
# 1. Reserve a number atomically
N=$(scripts/adr/next-free.sh --claim my-topic-slug)
# → e.g. "0532"
# A stub file docs/adr/0532-my-topic-slug.md.stub is created immediately.

# 2. Edit the stub into a real ADR
$EDITOR docs/adr/${N}-my-topic-slug.md.stub

# 3. Rename the stub to the final filename before committing
mv docs/adr/${N}-my-topic-slug.md.stub docs/adr/${N}-my-topic-slug.md

# 4. Create the one-row index fragment and register its order
$EDITOR docs/adr/_index_fragments/${N}-my-topic-slug.md
printf '%s\n' "${N}-my-topic-slug" >> docs/adr/_index_fragments/_order.txt

# 5. Regenerate and check all source-owned metadata
make docs-fragments-write
make docs-fragments-check

# 6. Stage the ADR, fragment, and generated outputs
git add docs/adr/${N}-my-topic-slug.md \
  docs/adr/_index_fragments/${N}-my-topic-slug.md \
  docs/adr/_index_fragments/_order.txt docs/adr/README.md \
  docs/adr/by-tag/ mkdocs.yml CHANGELOG.md
git commit -m "docs(adr): ADR-${N} my topic slug"
```

## Generated metadata

Edit ADR files and index fragments as the sources. The README index,
by-tag pages, and the sentinel-bounded ADR navigation block are rendered
outputs. Never correct those generated files by hand.

`make docs-fragments-write` regenerates the changelog, the ADR index,
then tag pages, then navigation. This order matters because navigation
reads the generated tag file set. `make docs-fragments-check` is read-only
and fails on missing, changed, or obsolete generated files. It also checks
that every ADR has one correctly named fragment, fragment ADR links resolve,
and the order manifest has no duplicate or missing-fragment entries.

The same check runs in the local pre-commit hook, required `Docs` CI job,
and Pages build. A clean MkDocs build alone does not prove metadata freshness.
Run `python3 scripts/docs/tests/test_generators.py` for the disposable
fixture tests. After a rebase that adds other ADRs, regenerate again from
the combined sources before committing.

Tags come from each ADR's front matter. Existing case/backtick normalization
is preserved, and repeated equivalent tags contribute one row per ADR.
Unsafe filename characters and the reserved tag `index` are rejected;
ordinary tags, including `c++`, underscore names, and version tags, retain
their spelling after normalization. The generator escapes title text for
Markdown tables without changing accepted ADR bodies or stripping meaningful
spaces from code examples. See [ADR-1242](../adr/1242-generated-adr-freshness.md).

## Why use `--claim`?

On a busy session day (e.g. 2026-05-18), 5 or more parallel Claude agents can all
call `scripts/adr/next-free.sh` within seconds of each other and receive the same
answer — because the read-only mode prints the next free number without reserving it.
That caused ~10 renumbers and rebases in a single session (ADR-0532).

`--claim` prevents this by:

1. Acquiring a POSIX `mkdir` lock (`/tmp/vmaf_adr_claim_lock_<repo>`) — atomic on
   Linux ext4/tmpfs — so concurrent callers on the same host serialize.
2. Writing a `docs/adr/NNNN-<slug>.md.stub` placeholder that all subsequent callers
   (including read-only mode) treat as a taken number.
3. Scanning remote branches via `git ls-remote --heads` + `git ls-tree` to also skip
   numbers already claimed by in-flight branches on origin.

## Commands

### Reserve a number

```bash
N=$(scripts/adr/next-free.sh --claim <slug>)
```

- `<slug>` must match `[a-z0-9][a-z0-9-]*` (lowercase letters, digits, hyphens).
- Prints the reserved 4-digit number on stdout.
- Creates `docs/adr/<NNNN>-<slug>.md.stub` on disk.
- Soft-fails on network outage (fetch errors are non-fatal); the pre-commit hook and
  CI gate (`adr-collision-check`) remain the hard backstop.

### Print the next free number (read-only, no claim)

```bash
scripts/adr/next-free.sh
```

Same as before ADR-0532. Does not create any file. Use this only for inspection;
do not use the printed number as the basis for hand-creating an ADR file without
immediately calling `--claim` (the number may be taken by the time you create the
file).

### Release an abandoned claim

```bash
scripts/adr/next-free.sh --release <NNNN>
```

Removes the stub file for `<NNNN>` and frees the slot. Use this if a PR is
abandoned and the stub was never promoted to a real ADR.

## Stub lifecycle

```text
--claim slug      stub created (NNNN-slug.md.stub)
                  ↓
edit stub         fill in ADR content in-place
                  ↓
mv stub → .md     rename before committing (git tracks the .md, not the .stub)
                  ↓
git commit        stub is gone; real ADR lives in tree
```

Stubs are **gitignored** by the pre-commit hook (it only fires on `.md` files), so
they do not pollute commit history. They are also excluded from `check-adr-numbering`
— only the final `.md` is validated.

## AGENTS.md invariant

All fork-local agents that create ADRs must call `--claim` before creating the file.
This is documented in the root `AGENTS.md` and enforced through `CLAUDE.md §12 r8`.
An agent that hand-picks a number without calling `--claim` will pass the local
pre-commit hook but may collide at the CI `adr-collision-check` gate.

## Running the smoke tests

```bash
bash scripts/adr/test-next-free.sh
```

The test suite covers sequential claims, parallel (race) claims, `--release`, and
invalid-slug rejection. All 12 assertions must pass. Run it after any change to
`scripts/adr/next-free.sh`.

## References

- [ADR-0386](../adr/0386-adr-numbering-collision-prevention.md) — original
  three-piece defence (hook + CI gate + helper script).
- [ADR-0535](../adr/0535-adr-atomic-allocator.md) — this atomic-claim extension.
- [docs/adr/README.md](../adr/README.md) — ADR index and conventions.
- [docs/adr/0000-template.md](../adr/0000-template.md) — ADR file template.
