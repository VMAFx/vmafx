<!-- markdownlint-disable MD013 -->
# ADR-1239: Preserve work during agent-state cleanup

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Lusoris
- **Tags**: workspace, agents, safety

## Context

`scripts/dev/cleanup-agent-state.sh` previously removed worktrees with `--force`
when their recorded owner PID had exited, without inspecting uncommitted files.
It also dropped stashes when their branch still existed. Neither condition
establishes that the work is redundant. A stash can contain the only copy of a
change on an existing branch, and an exited agent can leave valuable evidence.

## Decision

Make cleanup an inventory by default. Destructive worktree cleanup requires
`--apply` and exact registered paths selected with `--worktree`. Preserve
dirty, untracked, ignored, unreferenced detached, live-owner, unknown-owner,
main and current worktrees. Remove eligible checkouts through Git without
`--force`, retaining their branch refs. Report all stashes with their object
IDs and preserve them; stash retirement remains a separate reviewed action.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Existing PID/branch heuristic | Minimal interaction | Can discard unique files and stash changes | Ownership and branch existence do not prove redundancy |
| Default inventory and selected clean worktree removal | Preserves evidence and keeps a bounded cleanup operation | Operators must review exact targets and stop their writers | Chosen |
| Remove all cleanup support | Smallest safety surface | Leaves no supported path for retiring clean agent checkouts | Unnecessarily removes useful capability |

## Consequences

- **Positive**: running the documented command cannot silently discard work.
- **Negative**: old invocations without flags now report instead of removing;
  ignored build outputs must be reviewed separately before a checkout is eligible.
- **Neutral / follow-ups**: this utility does not classify code scaffolds,
  archive experimental evidence, delete branch refs, or change retention policy.
  Operators must keep selected worktrees idle during removal; PID and status
  checks cannot serialize unrelated processes that write to them.

## References

- `req`: "the repo needs a gc (send an agent for this)".
- `req`: "everything is in the right place... get the codebase clean".
- `req`: "scaffolds or stubs arent necessarily garbage".
- [Git worktree documentation](https://git-scm.com/docs/git-worktree),
  verified against the installed Git 2.55.0 manual.
- [Git stash documentation](https://git-scm.com/docs/git-stash),
  verified against the installed Git 2.55.0 manual.
