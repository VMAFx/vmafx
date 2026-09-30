<!-- markdownlint-disable MD013 MD060 -->
# Research-1383: Replaying real `docs/state.md` rebase conflicts through the three-way resolver

- **Status**: Active
- **Workstream**: `T-STATE-MD-RESOLVER-OURS-WINS-2026-09-30`; decision in [ADR-1383](../adr/1383-state-md-three-way-conflict-resolver.md)
- **Last updated**: 2026-09-30

## Question

`scripts/dev/resolve-state-md-conflict.py` was rewritten from "master's side wins" into a three-way merge of the index stages. On the conflicts this repository actually had, does the new tool produce what landed? Where it differs, which side is right? And how did the old rule do on the same conflicts?

## Method

- **Sample.** The 52 PRs squash-merged between 2026-09-20 and 2026-09-30 that touched `docs/state.md` (`git log --since=2026-09-20 -- docs/state.md`). For each, GitHub's GraphQL API gave the final head, the squash commit, and every `HeadRefForcePushedEvent`: 96 heads as they were before a force-push. A before-head is the branch before its author rebased it, so rebasing it again reproduces the conflict the author resolved.
- **Replay.** In a `python:3.14-slim` container with git: check out the head (sparse, `docs/state.md` only) and run `git rebase <squash>^`, the master just before the merge. At every stop on `docs/state.md`, run the resolver. Other conflicted files take the branch side, since only the ledger is compared. When the resolver exits 1, rerun it with the `--take` that matches what landed so the replay can continue, and record the stop. Then compare the final file with the squash commit's.
- **Comparison.** Rows, tombstones and disposition rows by key (section and text), all other lines as a multiset, and the order of keyed lines. Each key that differs is classified: *drift* when the branch itself changed that row after the replayed head (the head and the final head disagree on it), which the replay cannot know; otherwise by which side the result and the landed file agree with.
- **Baseline.** The same replay with the old resolver on the conflicted working file, with `check-state-md-rows.sh` run after each stop.

The final heads were almost all rebased onto the exact pre-merge master already, so rebasing them again stops nowhere. The conflicts come from the before-heads.

## Findings

### The window where master passes today's gate (#1566 to #1629)

33 replays stopped on `docs/state.md`, 50 stops in all.

| | Old resolver | Three-way resolver |
|---|---|---|
| Stops resolved and written | 50 | 42 |
| Stops refused (exit 1, nothing written) | 0 | 8 |
| Final file rejected by the row gate | 8 replays | 0 |
| Result identical to what landed, byte for byte | 7 | 11 |
| Keyed rows differing only through later branch edits (*drift*) | 47 | 34 |
| Keyed rows where the branch's change was lost or garbled | 51 (13 kept master's copy of a row the branch changed; 30 duplicate rows or disposition rows; 8 other) | 0 |
| Keyed-line order differs from what landed | 20 | 13 |

The old resolver's losses are silent more often than not. In the #1629 replays it kept master's Open copy of `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29` and dropped the branch's closed copy as an already-seen id, so the bug read as open with nothing for the gate to catch. Every "keep both" of a disposition row it made added a second row with the same label.

The eight refusals are real double edits, each naming what to decide:

- #1622 (two replays): the branch opened `T-SYCL-AOT-TARGETS-DROPPED-AT-LINK-2026-09-29` under Open bugs while master had already closed it (#1623).
- #1619 (four stops in two replays): master closed `T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29` (#1622) while the branch edited its Open row, and both sides rewrote the prose cell of the RC2 disposition row.
- #1600 (two replays): master renamed the "RC2 performance and backend acceleration" disposition row (ADR-1352) while the branch edited it.

The remaining differences from what landed:

- **Order (13 replays), by design.** A branch's new `_Updated` line and newly closed rows land after the ones master added (theirs after ours), where authors had put theirs on top. In #1622 the branch also swapped two disposition rows; the resolver keeps ours' order.
- **Later branch edits (drift).** 34 keyed rows and four `_Updated` or blank lines differ because the branch changed them after the replayed head.
- **A landed defect the resolver avoids.** #1625 landed two versions of its own `_Updated` line: a later commit rewrote the line an earlier commit added, and the old resolver kept both. The three-way replay keeps only the rewrite. Master still carries both lines.

### Found and fixed by the replay

The first full replay found two defects in the new resolver, both fixed with a test that fails without the fix:

- **A line both sides added, at different places, was kept twice.** #1619 was stacked on #1616 and replayed its commit, which master already had as a squash merge, so both sides added the same `_Updated` line. The resolver now keeps a non-blank line that both sides added once (`test_stacked_branch_replays_a_commit_master_squashed`).
- **Overlapping deletions were reported as a conflict.** In #1373 and #1507 both sides deleted `_Updated` lines over different, overlapping ranges, and the resolver refused the stop. Every line either side deleted now goes (`test_overlapping_deletions_are_not_a_conflict`).

### Outside the window

- **Before 2026-09-26** (27 replays, 31 stops): every master of that period fails today's gate on rows it learned to reject later, so the resolver exits 3 after writing, as the old resolver's result fails the gate too. Compared on content only, 13 results are identical to what landed and every differing row is drift. Two of those drift rows are a duplicate the replayed #1373 head itself carried. The 3 refusals are #1507 opening a row master had already closed.
- **#1561 and #1518** are integration branches with merge commits. Rebasing them replays dozens of already-merged commits (100 stops between them) and says nothing about the resolver.

## Limitations

- The `--take` choice in the replay knows what landed; it only lets the replay continue past a refusal.
- *Drift* is detected per key. A row the branch changed after the replayed head is not judged at all.
- Order is compared for keyed lines only; plain lines are compared as a multiset.
- The first replays ran in a Debian 12 image too, where `check-state-md-rows.sh` counted no id-bearing rows: that `mawk` (1.3.4 20200120) has no regex intervals, so the gate passed any file. This PR removes the intervals from the gate (`T-STATE-MD-ROWS-GATE-OLD-MAWK-2026-09-30`); the replays reported here ran on Debian 13, whose `mawk` was never affected, as was CI's Ubuntu runner.
