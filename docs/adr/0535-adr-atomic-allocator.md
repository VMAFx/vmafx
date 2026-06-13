<!-- markdownlint-disable MD013 MD060 -->
# ADR-0535: Atomic ADR Number Allocator with Cross-Branch Claim

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ci`, `docs`, `git`, `agents`, `tooling`

## Context

During the 2026-05-18 session, 22 PRs merged and approximately 10 ADR renumbers
were required because 5 or more parallel agents were all racing for the same next
available numbers (ADR-0509 through ADR-0530). Each renumber costs 5-10 minutes of
operator time, a rebase, and at least one additional CI round-trip. PR #1287's agent
surfaced the root cause directly:

> "ADR-0523, ADR-0528, ADR-0529 were each grabbed by parallel agents on master
> between when I picked them and when I tried to push. Required 3 renumbers + 3
> rebases."

ADR-0386 introduced a three-piece defence (helper script, pre-commit hook, CI gate)
that catches collisions after the fact but does not prevent them before the fact.
The existing `scripts/adr/next-free.sh` is read-only: it prints the next free
number but does not reserve it. Between the time an agent reads the number and the
time it pushes a branch, any number of other agents may have claimed the same number
on their own branches.

Two distinct races exist:

1. **Same-host race**: two Claude agents running concurrently on the same machine
   both call `next-free.sh` within the same second and get the same answer before
   either creates a file.

2. **Cross-branch race**: an agent cuts a branch, picks a number that was free at
   cut time, and pushes — but by push time another branch has merged to master and
   claimed the same number. The pre-commit hook cannot see origin/master; the CI
   gate catches it but only after the push.

ADR-0386 addressed collision detection. This ADR addresses collision prevention by
making the claim step atomic.

## Decision

We will extend `scripts/adr/next-free.sh` with a `--claim <slug>` mode that
atomically reserves the next free ADR number by creating a stub file
(`docs/adr/NNNN-<slug>.md.stub`) under a POSIX `mkdir`-based per-repo lock. The
full mechanism:

1. **Stub-file protocol**: a `.md.stub` file in `docs/adr/` acts as a cross-process
   reservation marker. `_collect_taken` now includes stub files alongside real `.md`
   files, so any subsequent caller (or `--claim` invocation) sees the reservation
   and skips the claimed number.

2. **POSIX `mkdir` lock** (`/tmp/vmaf_adr_claim_lock_<repo-key>`): `mkdir` is
   atomic on Linux ext4/tmpfs. The lock serialises concurrent `--claim` calls on
   the same host, ensuring two parallel agents in the same repo get distinct numbers
   even if they race within milliseconds. The lock is held only for the cheap
   in-memory computation + stub write; network I/O (git fetch) runs outside the lock.

3. **Remote-branch awareness**: before acquiring the lock, `--claim` fetches origin
   and walks every non-master branch tip via `git ls-remote --heads` + `git ls-tree`
   to collect ADR numbers claimed by in-flight branches. Those numbers are added to
   the taken set, so a new claim will skip them even if they are not yet on master.
   This is best-effort (soft failure on network outage); the CI gate and pre-commit
   hook remain the hard backstop.

4. **`--release <NNNN>`**: removes a stub file when a PR is abandoned, freeing the
   number for reuse.

5. **Read-only mode unchanged**: calling `next-free.sh` without arguments continues
   to work as before (prints the next free number without creating any file), for
   compatibility with existing scripts and documentation.

Agents are directed (via updated `CLAUDE.md`, `AGENTS.md`, `docs/adr/0000-template.md`,
and `docs/adr/README.md`) to call `--claim <slug>` rather than reading the number
and hand-creating the file.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Stub-file only (no lock) | Simple; cross-process persistence | Does not prevent race between two parallel calls that both read before either writes | Insufficient for the same-host race |
| POSIX `mkdir` lock only (no stub) | Serialises same-host callers | Lock is released immediately; the number is not persisted for cross-session callers or remote branches | Still races between sessions |
| Stub + `mkdir` lock (chosen) | Closes same-host race (lock) and cross-session/remote race (stub) | Slightly more implementation surface; stubs require manual cleanup via `--release` if PR is abandoned | Best trade-off; both races are covered |
| Central registry file (`docs/adr/.last-number`) | Single source of truth | Merge conflicts on every ADR PR; race-prone at checkout time | Trades one problem for another |
| Git notes / tags as reservation | No extra files | Requires push to origin before the number is visible; adds a network round-trip to the critical path | Slower and more fragile than the stub approach |
| GitHub Actions bot auto-assigns | Fully centralised; no local tooling | External app or complex token flow; still requires a human to trigger it; does not help same-host parallel agents | Disproportionate engineering cost |

## Consequences

- **Positive**: same-host parallel agents claiming ADR numbers simultaneously will
  get distinct numbers; the 2026-05-18 session's 10 renumbers would have been zero.
- **Positive**: in-flight branches on origin are also scanned, further shrinking the
  window for cross-branch collisions.
- **Positive**: stub files serve as a lightweight audit trail of in-progress ADR
  allocations visible to all processes sharing the working tree.
- **Positive**: backward-compatible — read-only mode is unchanged; existing
  `next-free.sh` invocations in docs and scripts continue to work.
- **Negative**: agents must remember to call `--claim` (rather than just read the
  number). Enforced via updated template and CLAUDE.md directive; the pre-commit
  hook and CI gate remain the hard backstop for non-compliant callers.
- **Negative**: abandoned claims leave `.md.stub` files on disk until `--release`
  is called. Stubs are gitignored (the pre-commit hook only fires on `.md` files),
  so they do not pollute the commit history; they do consume a slot in the number
  space until released.
- **Neutral / follow-ups**: update `docs/adr/0000-template.md` and `docs/adr/README.md`
  to direct authors to `--claim`; update `CLAUDE.md §12 r8` and `AGENTS.md` with the
  new invariant; add a smoke test (`scripts/adr/test-next-free.sh`) that exercises
  sequential and parallel claims.

## References

- Motivating event: 2026-05-18 session, ~10 renumbers during 22-PR merge train.
  PR #1287 agent comment: "ADR-0523, ADR-0528, ADR-0529 were each grabbed by parallel
  agents on master between when I picked them and when I tried to push."
- Prior defence layer: [ADR-0386](0386-adr-numbering-collision-prevention.md)
  (hook + CI gate; this ADR layers on top of it).
- Related: [ADR-0028](0028-adr-maintenance-rule.md), [ADR-0106](0106-adr-maintenance-rule.md),
  [ADR-0124](0124-automated-rule-enforcement.md).
- Source: user direction in session 2026-05-18 (`req`).
