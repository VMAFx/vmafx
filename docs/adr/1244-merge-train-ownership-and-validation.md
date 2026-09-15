<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1244: Guard merge-train ownership and exact-head validation

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: VMAFx maintainers
- **Tags**: ci, agents, safety, fork-local

## Context

The local merge train promoted and squash-merged stacked PR #1420 into its
unprotected parent branch #1396. Its ready queue, explicit rebase helper and
agent operator did not share the draft-promotion hold filter. The rebase helper
also reused an active agent checkout and continued to arming after a rejected
push. A missing required-check result was not positive release evidence.

This work belongs to the existing RC1 repository-hygiene initiative
`kp-354a7f673c1744d39bd69a80800278ff`. The affected watchdog, train and operator
were suspended; their old in-memory prompt is not an approved restart source.

## Decision

Use a tracked action gateway for train mutations. Require fresh open PR metadata
with base `master`; reject release PR #1213, held PRs and protected branch or
worktree owners at every mutation. Use unique detached train checkouts rather
than reusing another actor's checkout, bind pushes/merges to the observed head,
propagate failures, and preserve failed/dirty work instead of forced cleanup.
Arming or merging requires genuine required-check presence plus a locally
generated, exact-head receipt from executing full `make lint` and `make test`
on clean source. Inspection stays read-only; runtime installation and restart
are separate reviewed operator steps. The old unrestricted agent prompt is
retired, not resumed.

## Alternatives considered

| Option | Benefit | Risk | Decision |
| --- | --- | --- | --- |
| Add another draft-only hold filter | Small change | Ready/rebase/operator paths still bypass it | Rejected |
| Rely on GitHub branch protection | Useful last gate on master | Stacked branches can have no required checks; local worktree ownership is invisible | Insufficient alone |
| Share one tracked mutation gateway | Same ownership and validation rules at each action | Requires explicit runtime migration and local validation evidence | Chosen |
| Keep unrestricted agent mutation beside the gateway | Flexible autonomous repairs | Direct Git/gh calls bypass the gateway | Rejected; repairs use an explicit isolated-owner handoff |

## Consequences

- Non-master stacks and active helper branches cannot be auto-promoted, rebased
  or merged merely because GitHub reports them mergeable.
- A hold blocks all new train mutations; already-armed GitHub auto-merge must
  also be disabled when imposing a hold.
- Exact-head validation executes the two full gate commands; a handwritten
  pass flag or empty hosted-check list is not accepted as evidence.
- A failed rebase, rejected push or dirty disposable checkout is retained with
  its evidence. No unknown path is recursively deleted.
- This changes train control, not required-check membership, golden assertions
  or the numerical implementation. Suspended actors remain stopped until review.

## References

- [Agent worktree discipline](../development/agent-worktree-discipline.md).
- [Engineering principles](../principles.md).
- Incident: [PR #1420](https://github.com/VMAFx/vmafx/pull/1420).
- `req` (2026-09-08): “the repo needs a gc” and “we are getting close to rc1,
  it must be close to perfect”.
