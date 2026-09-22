<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1291: A reversal an accepted ADR mandates is declared in the tree, not in the PR body

- **Status**: Accepted
- **Date**: 2026-09-22
- **Deciders**: Lusoris
- **Tags**: `ci`, `docs`, `agents`

## Context

[ADR-1284](1284-silent-revert-detection-gate.md) made the silent-revert gate a
required check and gave it exactly one escape hatch: a `revert:` title or a
`reverts:` / `intentional revert:` line in the pull-request body.
[docs/development/silent-revert-gate.md](../development/silent-revert-gate.md)
stated the rule as "no annotation, no allowlist, no per-path exclusion", and
named the two legitimate shapes that trip the gate anyway — re-landing work an
earlier silent revert clobbered, and deliberately removing a fix.

On the zero-warning integration train both shapes arrived at once. Sixteen
findings are the `.corpus/` dataset root coming back after the `c2a3c7e0f`
mega-squash rewrote it to the retired numbered workspace directory, which
[ADR-1277](1277-workingdir-contract-cleanup.md) calls "the incorrect first
migration attempt" and a required contract check now enforces. One is the
[ADR-0759](0759-hip-adm-buffer-by-pointer.md) `buf_dev` tier returning after
`92ea978a4` silently reverted it — the very pair ADR-1284 was written from.
The PR-body escape hatch is the only mechanism available for them, and it is
per-PR and total: one line exempts every finding in the run, including any
genuine one that lands later in the same branch. On this branch it would have
exempted a real `docs/state.md` loss the gate had just caught.

Two detector defects inflated the same run. `reverse-hunk` reported every file
the branch deliberately **deleted**: for a target commit that only added the
file, every line it wrote is live, the merge removes all of them and puts
nothing back, which is exactly what the detector asks of a pure-addition
commit. And `resurrected` asked only whether a line was absent from the
target, although its own docstring defined it as "text the target had deleted,
coming back" — so every line a merge commit authored while resolving a
conflict was indistinguishable from a resurrection.

## Decision

We will keep the per-PR declaration for one-off reverts and add one in-tree
mechanism beside it: `scripts/ci/silent-revert-allowlist.json`, where a
reversal an accepted ADR mandates is declared as an entry naming the ADR, the
detector, the commit being undone, the exact paths, a regex every line of the
finding must match, and the reason. The gate downgrades a finding to a
`::notice` only when all of those agree, and reports everything else as before.
We will also repair the two detectors: `reverse-hunk` skips paths the merge
deletes outright, because `dropped` already distinguishes a deletion the
branch asked for from one it did not; and `resurrected` additionally requires
the line to be text the target once held and lost.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| In-tree entries keyed to ADR, commit, paths and evidence (chosen) | Each reversal is declared once, in a reviewed file, next to the decision that mandates it; an undeclared finding in the same run still blocks | One more file to keep honest as branches land | — |
| Declare it in the PR body, as ADR-1284 provides | No new mechanism | Per-PR and total: it exempts findings nobody has looked at, and this branch's run contained a real loss alongside the intended ones | Would have hidden the `docs/state.md` rows the gate caught |
| Subtract branch intent from `reverse-hunk`, as `dropped` and `resurrected` do | No allowlist at all | A rebased branch's intent *is* its effect, so the detector would go blind to the partial rewind it exists for — the `92ea978a4` shape | Removes the detection ADR-1284 was built for |
| Exclude the affected paths | Smallest diff | A path exclusion is permanent and silent: the next, unrelated reversal of those files passes too | Exactly the blanket disable the gate exists to prevent |
| Leave the gate red and merge past it | No code change | It is a required check, and a red gate teaches the train to ignore it | Defeats ADR-1284 |

## Consequences

- **Positive**: an intended reversal carries its justification in the tree, where a later reader finds it; a finding nobody has declared still fails even when it lands beside sixteen that are; the two detector defects no longer convert ordinary deletions and merge-authored text into findings.
- **Negative**: the allowlist is a suppression surface, and an entry written too loosely would hide work. The evidence regex and the mandatory `undoes` commit are what bound it; `scripts/ci/tests/test_check_silent_revert.py` pins that an entry naming another commit, another path, or only part of a finding's evidence declares nothing.
- **Neutral / follow-ups**: entries are not permanent. Once `master` carries the superseded state, the finding stops being produced and the entry should be removed with it.

## References

- [ADR-1284](1284-silent-revert-detection-gate.md) — the gate and its PR-body declaration.
- [ADR-1277](1277-workingdir-contract-cleanup.md) — the `.corpus/` split the sixteen findings implement.
- [ADR-0759](0759-hip-adm-buffer-by-pointer.md) — the `buf_dev` tier `92ea978a4` reverted.
- [docs/development/silent-revert-gate.md](../development/silent-revert-gate.md) — operator documentation.
