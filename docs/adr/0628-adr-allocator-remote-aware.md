<!-- markdownlint-disable MD060 -->
# ADR-0628: Remote-aware ADR number allocator — cross-worktree collision prevention

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: adr, tooling, ci, governance, agents, fork-local

## Context

On 2026-05-19, multiple parallel agents each running in an isolated worktree
collided on the same ADR numbers.  PR #1414 claimed ADR-0607; it merged before
PR #1415 (which had independently picked 0607), forcing a manual renumber to
0612.  PRs #1416 and #1417 both claimed ADR-0608 simultaneously; whichever
lands second will require a renumber.

Root cause: the existing allocator (`scripts/adr/next-free.sh`, introduced by
ADR-0535) serialises concurrent agents on the *same host* via a
`/tmp/vmaf_adr_claim_lock_*` directory lock and a `.md.stub` file in
`docs/adr/`.  But agents in *separate worktrees* — the mandatory isolation
model per `feedback_agents_isolated_worktree_only.md` — each have their own
working tree and only share the common `.git/` directory.  An agent in worktree
A cannot see the stub created by agent in worktree B unless B has pushed its
branch AND A has fetched it.  During the window between a claim and a push,
both agents see the same highest-taken number and independently increment from
it, producing identical candidate numbers.

Three concrete failure modes:

1. **Push-before-claim gap**: two agents start, both fetch master, pick the
   same next number, write stubs concurrently (the `/tmp` lock prevents this
   within the same PID namespace, but separate worktrees may use different
   `/tmp` mounts in container environments).
2. **Stale remote view**: an agent fetches origin before another agent has
   pushed its branch, so the remote scan sees no in-flight ADR.
3. **Claim-not-pushed**: an agent has written a stub but hasn't pushed — its
   claim is invisible to the network scan of any sibling agent.

## Decision

Extend `scripts/adr/next-free.sh --claim` with two complementary mechanisms:

1. **`.git/adr-claims/<NUMBER>` side-pointer** (cross-worktree, instant
   visibility): after writing the `.md.stub`, write a one-line file at
   `$(git rev-parse --git-common-dir)/adr-claims/<NUMBER>` containing the
   slug, timestamp, and branch name.  All worktrees sharing the same
   `.git/` common directory (the standard worktree layout) observe this file
   immediately, even before a push.  The lock key is also migrated to the
   common `.git/` directory so parallel agents in different worktrees of the
   same repo contend on the same lock.

2. **Remote-branch scan via `git ls-remote` + per-SHA `git ls-tree`** (cross-
   machine, covers pushed branches): before acquiring the lock, call
   `git ls-remote --heads origin` in the main shell (not a subshell) to get
   all open branch refs, then for each non-master SHA run `git ls-tree -r` to
   enumerate `docs/adr/NNNN-*.md` entries.  The union of master + local stubs
   dr-claims + remote-branch trees forms the taken-number set from which
   `max + 1` is computed.

3. **Offline fallback**: if `git ls-remote --heads` fails (network
   unreachable, no origin configured), set `REMOTE_OFFLINE=1` in the main
   shell, emit a `WARNING: could not reach origin — remote branch scan
   skipped` message to stderr, and continue with local + master + adr-claims
   only.  The number allocated is still collision-free among visible state.

4. **`--release` cleans both the stub and the side-pointer** so abandoned
   claims do not permanently block numbers.

The allocator continues to bias upward (max + 1) rather than filling gaps;
this ensures that a claim not yet pushed — but visible via adr-claims — is
never clobbered by a subsequent allocator run.

The CI `adr-collision-check` job in `rule-enforcement.yml` is extended to also
check each PR's added ADR numbers against all currently open PRs (via
`gh pr list --state open --json headRefName,number`), not just against master.
This catches cases where the local allocator was run offline or without a
network round-trip.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Server-side reservation (central DB or GitHub label) | Perfectly serialised across machines | Requires network for every claim; adds external dependency; complex to set up | Not suitable for offline dev workflows |
| Single shared worktree (no isolation) | No cross-worktree visibility problem | Violates ADR-0535 isolation rule; agents writing to same working tree cause checkout conflicts | Explicitly rejected by `feedback_agents_isolated_worktree_only.md` |
| Monotonic counter in a git note | Persists across forks without a DB | Git notes are easy to lose on rebase; requires force-push to update; racy without a lock | Too fragile; rebase notes do not solve the cross-worktree gap |
| Single-machine lock file in common `.git/` | Cheap; visible to all worktrees | Does not cover separate machines or container agents with different `.git/` mounts | Insufficient for the CI collision pattern (2026-05-19); combined with adr-claims as the intra-machine layer |
| Require all agents to push stubs before claiming | Perfectly visible via remote scan | Requires a push per claim (slow, noisy history); stub commits pollute the log | Unacceptable workflow friction |

## Consequences

- **Positive**: parallel agents on the same host sharing a `.git/` common dir
  will no longer collide; the adr-claims mechanism is instant (no network
  round-trip).  Agents on different machines or containers still benefit from
  the remote-branch scan covering already-pushed branches.  The CI gate now
  also catches collisions among in-flight PRs before merge.
- **Negative**: `--claim` now makes a network call (`git ls-remote`) that adds
  ~0.5–2 s to the claim path; the performance budget is < 5 s for 30 branches.
  Offline agents lose the remote-scan coverage (downgraded to a warning, not a
  fatal error).  The `adr-claims/` directory must be added to `.gitignore` /
  excluded from `git clean` to prevent accidental deletion.
- **Neutral / follow-ups**: the `.git/adr-claims/` directory is created inside
  the common `.git/` dir which git does not track, so no `.gitignore` entry is
  needed.  CI collision guard update requires `gh` CLI to be available in the
  runner (it is, on `ubuntu-latest`).

## References

- See [ADR-0386](0386-adr-numbering-collision-prevention.md) — original
  collision prevention ADR.
- See [ADR-0535](0535-adr-atomic-allocator.md) — the atomic allocator this
  ADR extends.
- Source: req — "Today multiple PRs collided on ADR-0607 and ADR-0608. Root
  cause: each agent's worktree has its own local view (master tip + local
  `.stub` files only). Other agents' open PRs in separate worktrees / remote
  branches are INVISIBLE."
- CI gate: `.github/workflows/rule-enforcement.yml` job `adr-collision-check`.
- Implementation: `scripts/adr/next-free.sh`, `scripts/adr/tests/test-next-free-remote-aware.sh`.
