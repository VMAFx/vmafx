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

# 4. Add an index row to docs/adr/README.md and commit
git add docs/adr/${N}-my-topic-slug.md docs/adr/README.md
git commit -m "docs(adr): ADR-${N} my topic slug"
```

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
