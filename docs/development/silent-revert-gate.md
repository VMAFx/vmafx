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
| `reverse-hunk` | the merge removes the lines a target commit added (those still live) and restores the ones it deleted | a partial rewind that leaves the rest of the file current |
| `dropped` | a target line is gone from the merged file and no non-merge commit of the branch removed it | a merge commit inside the branch that resolved against the target |
| `resurrected` | the merged file has a line the target does not, and no non-merge commit of the branch added it | deleted text coming back through a conflict resolution |

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
  that undoes `c2a3c7e0f`, and it does.
- **Deliberately removing a fix.** `1beb3b8a9`
  (`fix(ci): make test and scan gates fail closed`) removed the
  `ignore_outcome = true` that an earlier commit had added to
  `python/tox.ini`, and deleted that commit's changelog fragment with it.
  On-topic for the commit, invisible in its subject line.

Both are what the declaration is for.

## Declaring a revert

Say it where a reviewer reads it — a Conventional-Commit `revert:` title, or in
the PR body:

```text
reverts: #1234
intentional revert: re-lands the .corpus/ migration c2a3c7e0f clobbered
```

An unedited `intentional revert: REASON` template placeholder does not count.
A declaration turns the findings into `::notice` lines and the gate passes;
nothing is hidden, and the reasoning stays attached to the PR.

There is no in-tree suppression: no annotation, no allowlist, no per-path
exclusion. If a change removes target work, either the branch is wrong or the
PR body explains it.

## What it does not do

- It does not judge direction, and it does not know which side is correct.
- It does not find losses already on `master`. Those are catalogued in the
  read-only audit the gate was built from; this gate only stops the next ones.
- It needs full history. CI checks out with `fetch-depth: 0`; a shallow local
  clone will under-report, and the real-history fixture test skips rather than
  claim a pass.
