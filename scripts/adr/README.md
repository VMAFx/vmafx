<!-- markdownlint-disable MD013 MD060 -->
# scripts/adr — ADR number allocator

This directory contains the tooling for allocating ADR numbers without
collisions across parallel agents, worktrees, and remote branches.

## Quick reference

```bash
# Reserve the next free ADR number and create a stub file.
scripts/adr/next-free.sh --claim my-topic-slug
# → prints e.g. "0629"; creates docs/adr/0629-my-topic-slug.md.stub

# Print the next free number without claiming it (read-only).
scripts/adr/next-free.sh

# Release a previously claimed stub (abandoned PR).
scripts/adr/next-free.sh --release 0629
```

## How the allocator works

`next-free.sh --claim <slug>` atomically reserves a number by consulting
four sources of truth and returning `max(all-taken) + 1`:

| Source | What it covers |
|---|---|
| `docs/adr/NNNN-*.md` local files | ADRs committed to the local working tree |
| `docs/adr/NNNN-*.md.stub` local files | Numbers claimed in this worktree via prior `--claim` calls |
| `origin/master` (fetched) | All ADRs that have merged to master |
| `.git/adr-claims/<NUMBER>` | Numbers claimed by sibling agents in other worktrees sharing the same `.git/` directory |
| `git ls-remote --heads origin` + `git ls-tree` per branch | In-flight ADR files on pushed but unmerged remote branches |

The allocator **always returns `max + 1`**, never fills gaps.  This ensures
that a claim not yet pushed — but visible via `.git/adr-claims/` — is never
clobbered by a concurrent run.

### Cross-worktree claims (`.git/adr-claims/`)

When `--claim` succeeds it writes:

1. `docs/adr/<NUMBER>-<slug>.md.stub` — visible to anyone who has the same
   working tree checkout (in-worktree signal).
2. `.git/adr-claims/<NUMBER>` — visible to ALL worktrees that share the same
   common `.git/` directory (the standard `git worktree add` layout).  This
   is a plain text file containing `<slug> <ISO-timestamp> <branch>`.

The POSIX `mkdir`-based lock is keyed on the common `.git/` directory path,
so parallel agents in different worktrees of the same repo contend on the
same lock.

### Remote-branch scan

Before acquiring the lock, `--claim` calls `git ls-remote --heads origin` in
the **main shell** (not a subshell) so the offline flag propagates correctly.
For each non-master branch SHA, it runs `git ls-tree -r --name-only <SHA> --
docs/adr/` to enumerate ADR numbers already present on that branch.

**Performance**: the scan is bounded by the number of open branches.  On a
repo with 600+ ADRs and 30 open branches it completes in well under 5 seconds
(measured: ~30 ms on a warm repo).

### Offline / network-fail mode

If `git ls-remote --heads origin` exits non-zero (network unreachable, no
remote configured), the allocator:

1. Sets an internal `REMOTE_OFFLINE` flag.
2. Skips the remote-branch scan entirely.
3. Prints a `WARNING: could not reach origin — remote branch scan skipped`
   message to **stderr**.
4. Continues with local + master + `.git/adr-claims/` only and returns a
   number.

This means an offline claim can still collide with another agent on a
different machine that has also run offline — but it cannot collide with a
sibling agent in a different worktree on the same machine (`.git/adr-claims/`
covers that case).

### Releasing a claim

```bash
scripts/adr/next-free.sh --release <NNNN>
```

This removes both the `.md.stub` file and the `.git/adr-claims/<NNNN>`
side-pointer.

## CI collision guard

`adr-collision-check` in `.github/workflows/rule-enforcement.yml` runs two
phases on every PR:

- **Phase 1 — vs master**: fails if an ADR number added by this PR already
  exists on `origin/master` (catches post-merge renumbering failures).
- **Phase 2 — vs open PRs**: uses `gh pr list` to fetch every open PR's HEAD
  SHA and base branch, skips descendant stacked PRs whose `baseRefName` chain
  reaches the current PR branch, then checks independent PRs' `docs/adr/` trees
  for overlapping numbers (catches in-flight collisions before either PR
  merges).  Phase 2 is **best-effort**: if the `gh` call fails (rate-limit,
  token scope), it emits a warning but does not block the PR.

## Tests

```bash
# New acceptance tests (remote-aware behaviour):
bash scripts/adr/tests/test-next-free-remote-aware.sh

# Original smoke tests (sequential/parallel/release/slug-validation):
bash scripts/adr/test-next-free.sh
```

## Design references

- [ADR-0386](../../docs/adr/0386-adr-numbering-collision-prevention.md) — original collision-prevention policy
- [ADR-0535](../../docs/adr/0535-adr-atomic-allocator.md) — atomic allocator design (introduced `--claim`)
- [ADR-0628](../../docs/adr/0628-adr-allocator-remote-aware.md) — remote-aware extension (`.git/adr-claims/`, `ls-remote` scan, offline fallback, CI phase 2)
