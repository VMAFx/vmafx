# Silent-revert gate

`scripts/ci/check-silent-revert.py` answers one question about a branch:
**what would merging it remove from the target that it never set out to
touch?**

It runs in CI as the required check **Silent-Revert Guard** and locally as
`make silent-revert-check`. [ADR-1284](../adr/1284-silent-revert-detection-gate.md)
records why it exists.

## Run it before you open a PR

```bash
git fetch origin master
make silent-revert-check                                   # origin/master vs HEAD
make silent-revert-check BASE_REF=origin/master HEAD_REF=my-branch
python3 scripts/ci/check-silent-revert.py --base origin/master --head HEAD
```

Exit codes: `0` clean (or declared), `1` findings, `2` the analysis could not be
run — an unresolvable ref, unrelated histories, a merge that does not resolve
cleanly, or a git older than 2.38. The gate never prints "clean" for a case it
could not analyse.

## What it reports

A clean run says so and stops:

```text
check-silent-revert: clean — merging a8c9bcc93 into dcfc0d3f0 removes no target work.
```

A finding names the path, the detector, and — where it can — the commit being
undone:

```text
::error title=Silent revert (reverse-hunk)::core/src/feature/hip/integer_adm_hip.c:
  the merge undoes 31a51afb2 (perf(hip): pass AdmBufferHip by pointer...):
  44 line(s) that commit added are removed and 19 it deleted come back
```

## The four detectors

The gate analyses the **merge result** (`git merge-tree --write-tree`), never
the branch tree on its own, so a file the branch does not touch can never be
reported.

| Detector | Fires when | Catches |
| --- | --- | --- |
| `rewind` | the merged blob for a path equals an *older* blob that path held on the target's history, and the target's current blob differs | a whole file shipped in a stale copy |
| `reverse-hunk` | the merge removes the lines a target commit added (those still live) and restores the ones it deleted, and the file still exists in the merge | a partial rewind that leaves the rest of the file current |
| `dropped` | a target line is gone from the merged file and no non-merge commit of the branch removed it | a merge commit inside the branch that resolved against the target |
| `resurrected` | the merged file has a line the target does not have, the target once held and lost, and no non-merge commit of the branch added it | deleted text coming back through a conflict resolution |

`dropped` and `resurrected` are quiet for a cleanly rebased branch: there,
intent and effect are the same diff, and only history can show that the content
is old. That case is `rewind` and `reverse-hunk`'s job — and it is the case that
produced the fork's worst instance, `92ea978a4`.

`dropped` and `resurrected` check the surviving tree before reporting: a line
still present in the merged file (or already present in the target) was
relocated, not lost. Without that check they fire on a diff-alignment
artefact — inserting text above a line makes the cumulative `base..head` diff
re-pair that line as a delete plus an add, while no individual commit's diff
shows the pair.

Two boundaries are drawn where a detector would otherwise answer a question it
cannot answer. `reverse-hunk` skips a path the merge **deletes outright**: a
whole-file removal is the loudest hunk a review diff has, and what would make
it silent is the branch not having asked for it — which is what `dropped`
measures, since for a declared deletion every line is in the branch's own
removals and for an undeclared one none of them are. Without that boundary a
target commit that merely *added* a file made every later deliberate removal
of it read as a reverse-hunk against whoever wrote it. `resurrected` requires
the line to be text the target **once held and lost**, the other half of the
definition: a merge commit that writes its own conflict resolution produces
lines that are in the merge, absent from the target, and absent from every
non-merge branch commit, and those are new authorship, not recovered text.

Two more filters keep the set arithmetic honest. Rendered files (`CHANGELOG.md`,
`docs/adr/README.md`, `docs/adr/by-tag/`, `mkdocs.yml`, the tidy and standards
baselines) are skipped: a rewind there is a regeneration artefact, and the
generator's input is what matters. Conflict markers are never evidence —
`0c494cca0` once left three in `core/src/feature/cuda/integer_vif_cuda.c`, and
the PR that deleted them must not read as a revert.

## When the gate is right and the change is still correct

The gate reports that a merge undoes a commit. It cannot judge whether that is
the right outcome, and two legitimate shapes trip it:

- **Re-landing work an earlier silent revert clobbered.** `5a467e629`
  (`fix(governance): enforce local data lifecycle contract`) re-did the
  corpus-path migration (the retired private-state root to `.corpus/`) that
  the `c2a3c7e0f` mega-squash had reverted. As far as the gate is concerned
  that undoes `c2a3c7e0f`, and it does. The same holds for the
  [ADR-0759](../adr/0759-hip-adm-buffer-by-pointer.md) `buf_dev` tier coming
  back after `92ea978a4` — the pair this gate was built from — which the
  `resurrected` detector reports because master still carries the reverted
  state.
- **Deliberately removing a fix.** `1beb3b8a9`
  (`fix(ci): make test and scan gates fail closed`) removed the
  `ignore_outcome = true` that an earlier commit had added to
  `python/tox.ini`, and deleted that commit's changelog fragment with it.
  On-topic for the commit, invisible in its subject line.

Both need declaring. Which of the two mechanisms below applies depends on where
the justification lives.

## Declaring a one-off revert in the PR

Say it where a reviewer reads it — a Conventional-Commit `revert:` title, or in
the PR body:

```text
reverts: #1234
intentional revert: the fix regressed 10-bit input on the A380
```

An unedited `intentional revert: REASON` template placeholder does not count.
A declaration turns the findings into `::notice` lines and the gate passes;
nothing is hidden, and the reasoning stays attached to the PR.

This mechanism is **per-PR and total**: one line exempts every finding in the
run, including one nobody has looked at yet. Use it for a revert that is the
point of the pull request, not for a reversal that rides along inside a larger
branch.

## Declaring an ADR-mandated reversal in the tree

When an accepted ADR is what supersedes the reverted work, the declaration
belongs next to that decision rather than in one pull request's description.
Add an entry to [`scripts/ci/silent-revert-allowlist.json`](../../scripts/ci/silent-revert-allowlist.json)
([ADR-1291](../adr/1291-silent-revert-declared-reversals.md)):

```json
{
  "adr": "ADR-1277",
  "kind": "reverse-hunk",
  "undoes": "c2a3c7e0febd98a6831b9bfebe5e298e229b1244",
  "evidence": "(?:\\.workingdir\\d?|\\.corpus)/",
  "reason": "ADR-1277 names c2a3c7e0f's rename the incorrect first migration ...",
  "paths": ["ai/scripts/aggregate_corpora.py", "docs/ai/training-data.md"]
}
```

An entry is not a path exclusion. The gate downgrades a finding to a
`::notice` only when the detector, the exact path, the commit being undone and
**every** evidence line all agree, so an entry covers one migration and stays
silent about anything else that later touches the same file. `undoes` is
mandatory for a `reverse-hunk` entry — without it the entry would cover every
commit that ever wrote those paths. A malformed or unreadable allowlist exits
`2`: the gate does not guess in either direction.

Entries are not permanent. Once the target carries the superseded state the
finding stops being produced, and the entry goes with it.

There is still no unreviewed suppression: no in-code annotation, no bare path
exclusion, and no way to silence a finding without naming the decision that
justifies it.

## What it does not do

- It does not judge direction, and it does not know which side is correct.
- It does not find losses already on `master`. Those are catalogued in the
  read-only audit the gate was built from; this gate only stops the next ones.
- It needs full history. CI checks out with `fetch-depth: 0`; a shallow local
  clone will under-report, and the real-history fixture test skips rather than
  claim a pass.
