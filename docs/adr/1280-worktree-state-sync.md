<!-- markdownlint-disable MD013 MD060 -->
# ADR-1280: Synchronize private state through regular worktree mirrors

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Kilian, Codex
- **Tags**: workspace, agents, git, hooks

## Context

The post-commit hook ran `praetorctl state sync .` in whichever checkout
created the commit. Linked worktrees intentionally do not share the ignored
`.workingdir` tree, so that command failed for an isolated agent. Pointing the
worktree at the main checkout with a symlink is not an alternative: Praetor's
confinement check rejects a symlinked state root. Running the command against
the main checkout instead records that checkout's branch, HEAD, and dirty
state rather than the committing worktree's identity.

Private state still needs one canonical authority. Independent worktree
ledgers would fragment the bug and decision history, while a concurrent hook
must not overwrite another hook's update.

## Decision

The post-commit hook will call `scripts/githooks/state-sync.sh`. A linked
worktree receives regular-file copies of the six canonical ledgers under its
own ignored `.workingdir`, runs Praetor there, and atomically copies only the
derived `STATE.md` result back to the canonical checkout. The operation holds
an exclusive lock in Git's common directory and fails closed on contention,
missing or non-regular canonical ledgers, a symlinked local state root, or an
unavailable synchronizer. Existing worktree-local cache content is preserved.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Symlink each worktree's state root to the canonical tree | No copying; every command sees one tree | Praetor rejects the confinement root; worktree commands can mutate canonical state without serialization | Invalid under the installed state tool's safety contract |
| Run every hook against the main checkout | No mirror and one authority | Records the wrong branch, HEAD, and dirty state; depends on unrelated main-checkout state | Produces false governance evidence |
| Keep independent state in every worktree | Correct local Git identity | Splits bugs, questions, and backlog across ephemeral worktrees | Loses the single durable authority |
| Regular mirror plus common-Git lock | Correct Git identity, one authority, portable locking | Copies six small files and leaves a recoverable stale lock after an untrappable process death | Chosen; it preserves both confinement and provenance |

## Consequences

- **Positive**: Agent commits synchronize the branch and HEAD that actually
  produced them, while canonical private state remains centralized. Symlink
  and concurrent-update failures block visibly instead of being ignored.
- **Negative**: An untrappable process death can leave the lock directory for
  manual inspection and removal. Each linked worktree retains a small ignored
  mirror of the ledgers.
- **Neutral / follow-ups**: The disposable hook fixture pins regular-file
  mirrors, cache preservation, correct branch identity, copy-back scope, and
  symlink refusal. This changes no libvmaf or FFmpeg surface.

## References

- [ADR-0332](0332-agent-worktree-drift-hard-guard.md) — isolated agent
  worktrees are mandatory.
- [ADR-1241](1241-worktree-hook-dispatch.md) — hooks resolve the active
  worktree at invocation time.
- [Research-1280](../research/1280-worktree-state-sync.md) — reproduced
  failure modes and regression evidence.
- Source: `req` — “no warning or error is just ignored because of being og
  netflix code, fix them all”.
