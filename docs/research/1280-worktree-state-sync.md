# Research-1280: Worktree-safe private-state synchronization

## Failure reproduction

The existing post-commit command ran `praetorctl state sync .`. In the linked
`workingdir-contract-clean` worktree it had no private ledgers to read. A
temporary symlink from that worktree's `.workingdir` to the canonical checkout
was rejected by Praetor's confinement validation. Running the same command
with the canonical checkout as its target succeeded but wrote that checkout's
branch, HEAD, and dirty state into `STATE.md`, not the committing worktree's.

The installed `praetorctl state bug add --help` and `state sync` behavior were
checked directly on 2026-09-21. The state parser also proved that the canonical
ledgers must remain regular files; a symlink workaround would weaken the tool's
path boundary rather than fix the hook.

## Selected contract

Git exposes one common directory to the main checkout and all linked
worktrees. The helper uses that directory for an exclusive lock and derives
the canonical checkout from it. It copies `OPEN.md`, `BACKLOG.md`, `BUGS.md`,
`QUESTIONS.md`, `STATE.md`, and `bugs.meta.json` into a regular ignored
worktree directory, runs the synchronizer from that worktree, then atomically
publishes only the derived state summary. Cache and evidence directories are
not mirrored or removed.

A directory lock was selected over a platform-specific `flock` dependency so
the Bash hook keeps working in Git-for-Windows and macOS environments. A stale
directory is deliberately visible and fail-closed after an untrappable crash;
automatic lock stealing could overlap a still-running synchronizer.

## Regression evidence

`python3 scripts/githooks/tests/test_install.py` creates a real linked
worktree and a controlled Praetor double. The regression asserts:

- the mirror and every ledger are regular files;
- the linked worktree branch is recorded in canonical `STATE.md`;
- unrelated canonical ledgers are unchanged;
- an existing worktree-local cache file survives synchronization; and
- replacing the local state directory with a symlink fails without changing
  canonical state.

A live run from the contract worktree recorded branch
`fix/workingdir-contract-cleanup` at commit `204591c47` and passed
`praetorctl state sync -verify .`. This is tooling-only evidence; it does not
claim a libvmaf build or score result.
