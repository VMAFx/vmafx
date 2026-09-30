<!-- markdownlint-disable MD013 MD060 -->
# ADR-1385: The hook installer leaves lefthook's hooks in place

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: ci, tooling, workspace, agents, git-hooks, windows

## Context

[ADR-1249](1249-praetor-governance-adoption.md) gave lefthook the `pre-commit`
and `pre-push` hooks and left `commit-msg` and `pre-rebase` with the
[ADR-1241](1241-worktree-hook-dispatch.md) dispatchers. It also recorded that
`make install-hooks` refuses to run once lefthook owns a hook, because the
installer treats every file it did not write as a custom hook, and it left open
whether the installer should learn lefthook or move lefthook's installation
behind it. Until that is settled no checkout can hold the split ADR-1249
describes: installing lefthook first makes `make install-hooks` stop at its
`pre-commit` shim, and installing the dispatchers first is refused wherever Git
LFS has installed its own `pre-push` hook, as it has on the Windows host below.

The question became concrete when lefthook was first installed on a Windows
workstation ([Research-1385](../research/1385-lefthook-windows-host.md)): the
checkout needs lefthook's governance hooks and the framework's commit-message
check at the same time.

## Decision

`scripts/githooks/install.py` recognises a hook that lefthook wrote (its shim
calls `call_lefthook run`) as owned by lefthook: it neither refuses nor replaces
it, installs its dispatchers for the remaining hooks, and says which hooks it
left in place. The supported order is `lefthook install` first, then
`make install-hooks`. Lefthook's own configuration is unchanged; it still
declares only `pre-commit`, `pre-push` and `post-commit`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Installer skips lefthook shims (chosen) | Implements ADR-1249's split as written; one small predicate; both hook managers keep their own install commands | Two install commands, in a fixed order | — |
| `make install-hooks` runs `lefthook install` itself | One command | The installer would own lefthook's `.old` renaming and its failure modes, and a missing lefthook binary would block the framework hooks as well | Couples two independent tools for a one-line saving |
| Move `commit-msg` and `pre-rebase` into `lefthook.yml` | One hook manager for every hook | Reverses ADR-1249's ownership split and ADR-0924's native-mode escape hatch; `pre-rebase` would lose the dispatcher's worktree-independence tests | A larger policy change than the defect needs |
| Keep refusing and document `pre-commit install --hook-type commit-msg` | No code change | No `pre-rebase` worktree-drift guard; a framework hook written by hand is exactly what ADR-1241 replaced | Loses the agent drift guard on the host that runs most agents |

## Consequences

- **Positive**: a checkout gets lefthook's governance hooks, the framework's
  `commit-msg` validation and the `pre-rebase` drift guard together.
- **Negative**: the order matters. Running `make install-hooks` first still stops
  at any hook it does not recognise, such as Git LFS's `pre-push`.
- **Neutral / follow-ups**: `scripts/githooks/tests/test_install.py` covers the
  shim case; [Local Git hooks](../development/pre-commit-hooks.md) documents the
  order and the Windows setup.

## References

- [ADR-1249](1249-praetor-governance-adoption.md) — the ownership split and the
  open follow-up this decides.
- [ADR-1241](1241-worktree-hook-dispatch.md) — the dispatchers and their custom-hook
  refusal.
- [Research-1385](../research/1385-lefthook-windows-host.md) — the Windows host
  measurements.
- Source: orchestrator brief, 2026-09-30 ("make the local lefthook + pre-commit
  hook stack pass on this Windows host ... so the hooks can be installed here").
