# ADR-1284: Gate merges on the work they would remove from the target

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: `ci`, `agents`, `workspace`

## Context

A pull request can remove work from `master` with nothing in its review diff
looking like a removal of somebody else's commit. `92ea978a4` (#102,
`perf(cuda): ciede 8/16bpc — __ldg() read-only cache routing`) reset three HIP
ADM files to the exact blobs they held before `31a51afb2` (#101, ADR-0759,
`perf(hip): pass AdmBufferHip by pointer`), merged 47 seconds earlier. The three
post-#102 blobs are byte-identical to the pre-#101 blobs; the 50 lines #101 added
to `integer_adm_hip.c` are the 50 lines #102 removed. Every check was green, and
because #101 stayed in `git log` and its ADR, `AGENTS.md` note and `docs/state.md`
row stayed in tree, four months of later audits read the change as shipped. It
was rediscovered only as a HIP performance question and restored in PR #1481.

That is one instance of a class. The read-only audit in
`.workingdir/evidence/silent-reverts-2026-09-18.md` triaged about 133 candidate
pairs from master's first-parent history and confirmed dozens of live losses,
including whole features (ADR-0753 resolution-aware CUDA dispatch, absent while
the docs describe it), GPU option parity across ten extractor twins, and
security hardening that had to be written twice. The mechanism is not GitHub's
squash merge by itself: the reverting branches carried the old file contents in
their own diff — a rebase conflict resolved by taking the stale side, or a
squash built from a stale worktree. No gate in the fork asked what a merge
removes from its target, so nothing failed.

## Decision

We will add `scripts/ci/check-silent-revert.py` and run it as the required CI
context `Silent-Revert Guard`. It measures the real merge result
(`git merge-tree --write-tree`), not the branch tree in isolation, and fails when
that merge removes work from the live target that the branch never set out to
touch. Four detectors run together: `rewind` (a merged blob equal to an older
blob of that path on the target's history), `reverse-hunk` (a specific target
commit undone hunk-for-hunk), `dropped` and `resurrected` (lines moved by a
conflict resolution rather than by any non-merge commit of the branch). A
deliberate revert is declared — a `revert:` Conventional-Commit title, or
`reverts: #N` / `intentional revert: <reason>` in the PR body — never suppressed
by an in-tree annotation. The gate fails closed: an unresolvable ref, unrelated
histories, a conflicting merge or a git without `merge-tree --write-tree` exit
non-zero rather than print "clean".

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Required pre-merge gate over the merge result (chosen)** | Catches the defect before it lands; measures what actually merges; names the commit being undone | Needs a declaration on the ~7% of PRs that legitimately re-land or remove recent work | — |
| Extend `~/.cache/vmafx-tools/check-resurrected.py` only | Already exists; the same set arithmetic | Its reference is the *pre-restack branch*, which no longer exists at PR time; it is quiet for the rebased-branch shape that produced #102 | Its principle is kept as the `dropped` / `resurrected` detectors, but it cannot stand alone |
| Compare the branch tree against the target (`git diff base..head`) | Trivial to implement | Reports every file the target changed that the branch did not touch — noise on any branch that is behind | Rejected: a gate nobody can keep green is not a gate |
| Periodic audit instead of a gate | No per-PR cost | The 2026-09-18 audit is exactly that, and it ran four months late, after the losses compounded | Detection has to be pre-merge |
| Forbid squash merges / require linear rebase | Removes one carrier | `92ea978a4` was already a rebased branch; rebasing is where the bad resolutions happen | Does not address the observed mechanism |

## Consequences

- **Positive**: a merge that removes target work now has to say so, in the PR
  where a reviewer sees it. The report names the commit being undone, so the
  question "is this deliberate?" is answerable without a bisect.
- **Negative**: re-landing work that an earlier silent revert clobbered is
  itself a revert as far as the gate is concerned, and needs a one-line
  declaration. Measured over master's last 30 first-parent commits, two
  (`5a467e629`, `1beb3b8a9`) would have needed one, both correctly.
- **Negative**: an unmergeable PR fails the gate instead of being analysed.
  That is deliberate — the merge result is what the gate measures, and there
  is not one until the branch is rebased.
- **Neutral / follow-ups**: the gate cannot judge *direction*. It reports that
  a merge undoes a commit; a human decides whether that is the right outcome.
  The existing losses catalogued in the evidence ledger are not fixed by this
  ADR — the gate only stops the next ones.

## References

- `.workingdir/evidence/silent-reverts-2026-09-18.md` — the read-only audit of
  master's first-parent history this gate is built from.
- `docs/research/research-1284-silent-revert-detection.md` — mechanism proof, detector
  design and the false-positive measurement.
- `docs/development/silent-revert-gate.md` — usage.
- [ADR-0165](0165-state-md-bug-tracking.md) (`docs/state.md` discipline),
  [ADR-0221](0221-changelog-adr-fragment-pattern.md) (fragment rendering),
  [ADR-0759](0759-hip-adm-buffer-by-pointer.md) (the reverted decision).
- Commits: `31a51afb2` (#101), `92ea978a4` (#102), restoration PR #1481.
- Source: `req` — the user directed that the mechanism behind the silent-revert
  cluster be demonstrated on a real case and closed with a fail-closed gate,
  not restated as theory.
