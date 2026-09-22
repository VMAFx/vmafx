# Research-1284: detecting merges that silently revert the target

**Date**: 2026-09-21
**Status**: Completed — gate in `scripts/ci/check-silent-revert.py` (ADR-1284)
**Related**: `.workingdir/evidence/silent-reverts-2026-09-18.md`, ADR-0759, PR #1481

## 1. The mechanism, proved on a real pair

`31a51afb2` (#101, `perf(hip): pass AdmBufferHip by pointer in ADM kernels (F3
fix, ADR-0759)`, 2026-05-29 11:39:56) and `92ea978a4` (#102, `perf(cuda): ciede
8/16bpc — __ldg() read-only cache routing`, 11:40:43) are parent and child on
master's first-parent history. 47 seconds apart, subjects with nothing in
common.

`git show --stat` on each:

| Path | #101 | #102 |
| --- | --- | --- |
| `core/src/feature/hip/integer_adm/adm_cm.hip` | `20 +++---` | `20 +++----` |
| `core/src/feature/hip/integer_adm/adm_csf.hip` | `24 ++----` | `24 ++++---` |
| `core/src/feature/hip/integer_adm_hip.c` | `70 +++++---` | `70 ++++-------` |

The subject of #102 names CUDA ciede. The blobs settle it:

```console
$ git rev-parse 31a51afb2^:core/src/feature/hip/integer_adm_hip.c   # before #101
baf2b339816b6143cbcae62c1cf8cb9eddf612f1
$ git rev-parse 31a51afb2:core/src/feature/hip/integer_adm_hip.c    # after #101
bbd11cbf0917b3abc542b7f63e1202df60efffae
$ git rev-parse 92ea978a4:core/src/feature/hip/integer_adm_hip.c    # after #102
baf2b339816b6143cbcae62c1cf8cb9eddf612f1
```

All three HIP files are byte-identical after #102 to what they were before
PR #101; `git diff 92ea978a4 31a51afb2^ -- <path>` is empty. The 50 lines #101
added to `integer_adm_hip.c` are the 50 lines #102 removed.

**The mechanism is not GitHub's squash merge on its own.** A 3-way merge of a
branch that never touched those files keeps the target's copy. For the merge
result to carry the pre-#101 blobs, the branch's *own* tree had to carry them —
a rebase conflict resolved by taking the stale side, or a squash built from a
stale worktree. The evidence ledger reached the same conclusion independently
("the reverting PRs carried old file contents as part of their own diff").

The revert survived because nothing pointed at it: #101's commit, its ADR-0759,
its `core/src/feature/hip/AGENTS.md` note and its `docs/state.md` row all stayed
in tree. `BUG-035` filed the contradiction as a documentation error ("AGENTS.md
claims ADR-0759 moved `AdmBufferHip` to pass-by-pointer; the kernels still take
it by value") months before anyone found the commit that undid it.

## 2. Why the existing tool does not cover it

`~/.cache/vmafx-tools/check-resurrected.py` compares a restacked branch against
its pre-restack self: every line the new branch adds relative to its new base
must be a line the old branch added relative to its old base. The principle is
right, but its reference — the branch as it stood before the restack — does not
exist at PR time. For a branch rebased onto current master (which #102's was:
merge base equals base), that check sees intent and effect as the same diff and
reports clean.

The gate keeps the principle as two of four detectors (`dropped`,
`resurrected`, measured against the union of the branch's *non-merge* commits,
so a bad conflict resolution is not counted as intent) and adds two that work
against history instead of against intent:

- `rewind` — the merged blob for a path equals an older blob that path held on
  the target's history while the target's current blob differs.
- `reverse-hunk` — per target commit `M` touching the path: the merge removes
  every line `M` added that is still live in the target, and restores every
  line `M` deleted.

Both are computed on `git merge-tree --write-tree base head`, the tree GitHub's
squash would commit, so a path the branch does not touch is never reported.

## 3. Discriminating a revert from a rewrite

The first draft of `reverse-hunk` fired on `04c6610ae` (`fix(core): clear
high-signal native diagnostics`), which rewrote the file-header comment
`b14a718ed` had written in `core/tools/vmaf.cpp`: 7 of that commit's added
lines removed, 1 of its deleted lines restored. A rewrite of freshly added code
and a rewind of it look the same from one side.

The predicate now needs `MIN_EVIDENCE_LINES = 2` on **both** sides — at least
two of `M`'s added lines removed *and* at least two of its deleted lines
restored — or, for a pure-addition `M`, that the merge puts nothing at all in
its place. Lines shorter than six characters or without an alphanumeric are not
evidence (`}`, `*/`, `#endif` match everywhere), and conflict markers are
excluded outright: `0c494cca0` left three in
`core/src/feature/cuda/integer_vif_cuda.c` and the PR that deleted them reset
the file to its pre-marker blob, which the first draft called a rewind.

Cost of the tightening: a revert of a commit that deleted exactly one line is
not caught by `reverse-hunk` when the merge also adds new text to that file.
`rewind` still catches it when the whole file is rewound.

## 4. False-positive measurement

Replayed over master's last 30 first-parent commits (`base = C^`, `head = C`),
about 1 s per commit:

| Draft | Fired | True positives |
| --- | --- | --- |
| before tightening | 3 / 30 | 2 |
| after tightening | 2 / 30 | 2 |

The two survivors are correct and would each need a one-line declaration:

- `5a467e629` (`fix(governance): enforce local data lifecycle contract`)
  re-lands the corpus-path docstring migration (the retired private-state
  root to `.corpus/`) that the
  `c2a3c7e0f` mega-squash — itself one of the ledger's worst offenders — had
  reverted. Undoing a silent revert is still a revert.
- `1beb3b8a9` (`fix(ci): make test and scan gates fail closed`) removes the
  `ignore_outcome = true` an earlier commit added to `python/tox.ini`, on
  topic for its subject, and deletes that commit's changelog fragment
  (`changelog.d/fixed/arm-ci-tox-coverage-no-data.md`) with it. Both the
  fragment and its rendered `CHANGELOG.md` entry are gone at HEAD; the
  fragment's own text ("coverage combine", "No data to combine") returns no
  hit. Whether erasing a released changelog entry was intended is a question
  the gate surfaces and a human answers.

Neither is a defect in the gate. Both are merges that remove target work, which
is exactly what it says.

## 5. The diff-alignment artefact

Dogfooded against a 48-commit stack (`origin/master` `371ff5891` to
`98d6a569e`), `dropped` and `resurrected` both reported the same single line
of `python/test/executor_test.py`: `class ExecutorTest(unittest.TestCase):`,
which is present exactly once in **both** trees. No commit in the range
removed it (`git log -S` finds none) and the range has no merge commits.

The cause is diff alignment, not a lost line. One commit inserts a new class
above it, so the cumulative `base..head` diff pairs the old
`class ExecutorTest(...)` against the new `class SerializationProbeExecutor(...)`
and emits a delete plus an add, while that commit's own diff shows only
insertions. Intent and effect are then computed from differently-aligned
hunks and the subtraction leaves a residue.

Fix: both detectors now check the surviving tree. `dropped` reports a line
only when it is absent from the merged blob, `resurrected` only when it is
absent from the base blob. That states what the detectors mean — *lost* and
*reappeared* — instead of inferring it from hunk alignment, and it costs
nothing: the merge-resolution fixture still fails, because the line it drops
really is gone from the merge result. After the fix the 48-commit stack
reports 18 findings, all `reverse-hunk`, all true (the `.corpus/` re-landing
and the two deleted changelog fragments), and the 30-commit sweep is
unchanged at two.

## 6. Fail-closed behaviour

Exit 2, never "clean", for: an unresolvable ref, no merge base between the two
histories, a merge that does not resolve cleanly (there is no merge result to
measure), and a git without `merge-tree --write-tree` (< 2.38). The fixture
suite `scripts/ci/tests/test_check_silent_revert.py` asserts each of those,
plus the reproduced revert, the merge-commit resolution case, four
must-stay-clean shapes, the declaration path, the rejected template
placeholder, and a replay of the real `31a51afb2` / `92ea978a4` pair that skips
rather than passes when the commits are not in the clone.

## 7. What this does not solve

The gate is pre-merge only. The losses already catalogued in the evidence
ledger — ADR-0753's resolution-aware CUDA dispatch, the GPU option-parity
cluster, the strict-JSON family, the dropped rebase notes — are unaffected;
restoring them is separate work tracked there. And the gate cannot judge
direction: it reports that a merge undoes a commit, not which side is right.
