<!-- markdownlint-disable MD013 MD060 -->
# ADR-1383: Resolve `docs/state.md` rebase conflicts three-way, keyed by bug id

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: process, state-hygiene, tooling, git, fork-local

## Context

[ADR-0165](0165-state-md-bug-tracking.md) makes `docs/state.md` the bug ledger. Its rows move from "## Open bugs" to "## Recently closed", so `.gitattributes` keeps it out of `merge=union`, and nearly every rebase of a branch that touches it stops on a conflict. `scripts/ci/check-state-md-rows.sh` rejects the results a careless resolution produces: a duplicated id, a row whose status disagrees with its section, a row still open next to its own move tombstone.

PR #1389 added `scripts/dev/resolve-state-md-conflict.py` with one rule: master's side of each conflict hunk wins, and the branch contributes only rows whose id master does not have. Mid-rebase, though, "ours" is master **plus the branch commits already replayed**, and a hunk sees neither moves nor anything outside the hunk. On 2026-09-30 the rule produced files the gate rejected three times:

- a branch that closed a bug moved its row to "## Recently closed" while master's untouched Open copy survived, a duplicate id (#1627 `T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29`, #1629 `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`);
- a later commit of the same branch rewrote a row an earlier commit added, and "ours" (the earlier commit, already replayed) won with the stale text (#1625 `T-SYCL-ADM-DECOUPLE-K-INT32-WRAP-2026-09-29`; master still carries both versions of that PR's `_Updated` line);
- both sides added `_Updated` lines and rows at the top of "## Recently closed", which must all be kept.

The disposition table under "## First-release phase classification" (ADR-1341, ADR-1352) adds a fourth failure: nearly every PR adds its ids to one of its rows, so a line-level merge turns every rebase into a conflict, and keep-both left master with two "RC3 performance and backend acceleration" rows after #1626.

## Decision

The resolver reads the three versions git keeps in the index for the conflicted path (`:1:` base, `:2:` ours, `:3:` theirs) and merges them three-way; it never parses conflict markers.

1. **Rows and move tombstones are records keyed by bug id**, in the shapes `check-state-md-rows.sh` recognises. A record's state is its text plus its `##` section, or absent. Same on both sides: keep it. Unchanged on one side: the other side's state wins, which carries an edit, a move, a close or a deletion. Changed differently on both sides: a conflict.
2. **Disposition rows are records keyed by their bold label.** When both sides changed one, the id list merges as a set (ours, plus the ids theirs added, minus the ids either side removed; ours' order, then theirs' additions) and every other cell three-way by text. Rows repeating a label on one side are folded into one (union of ids) before the merge, and the tool reports it. An id that ends up in two disposition rows is reported and fails the run after writing.
3. **Every other line merges three-way by line.** Lines both sides add at one point are all kept, theirs after ours; a non-blank line both sides added anywhere is kept once (a branch stacked on a PR that master already squash-merged replays that PR's `_Updated` line). Placement keeps ours' order; a line only theirs has goes after its nearest theirs neighbour still present in the same section.
4. **Conflicts fail closed.** The tool writes nothing, names every conflicting id, label or line with its three versions, and exits 1. `--take NAME=ours|theirs` settles one conflict explicitly: a bug id takes that side's row and tombstone, a label that side's disposition row, a `line:N` handle that side's lines.
5. **The tool verifies its own output.** It writes LF bytes, runs `check-state-md-rows.sh` on the result and exits 3 when the gate rejects it.
6. **Its test suite runs real rebases in CI.** `scripts/dev/test-resolve-state-md-conflict.py` builds throwaway repositories, stops real `git rebase` runs on `docs/state.md` and checks every result against the gate. It runs in the `state.md row hygiene (ADR-0165)` step of `rule-enforcement.yml` and under `scripts/ci/test_git_fixture_isolation.py`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Three-way merge of the index stages, keyed by id and label (chosen) | Sees moves across sections and history across replayed commits; only a real double edit stops the rebase; the decision per row is explainable from three versions | More code than a hunk rule; a record changed on both sides still needs a human | — |
| Keep "master's side wins, the branch adds unseen ids" (PR #1389) | Tiny; right when the branch only adds a row | Wrong whenever the branch moves, edits or deletes a row, or rewrites its own earlier commit; produced three gate failures in one day | The failures are the reason for this ADR |
| "The branch's side wins" | Right for the branch's own moves and rewrites | Drops every row master added or changed inside the hunk | Symmetric to the old failure |
| `merge=union` plus a de-duplication pass | Automatic; no tool to run | Union cannot tell which copy of a moved row is current; ADR-0165 excludes state.md from union for that reason | Guessing the newer copy is what the gate exists to prevent |
| A custom git merge driver (`merge=state-md` in `.gitattributes`) running the same algorithm | Resolves without a manual step | Needs `git config merge.state-md.driver` in every clone (fresh agent worktrees and CI checkouts lack it, and GitHub's merge never runs it), so the same rebase behaves differently per clone; a driver that fails falls back to markers silently | A later ADR can register the tool as a driver; the explicit command stays the one documented path |
| Line-level three-way only (git's own merge, or diff3 on every line) | No new rules | Cannot see a move, and conflicts on every disposition row both sides touched | Does not remove the conflicts that need removing |

## Consequences

- **Positive**: closing, editing and deleting rows on a branch survive a rebase; a branch's later commits keep their rewrites; disposition rows no longer conflict when two PRs add ids to the same phase; duplicate disposition rows fold back into one on the next rebase that touches them; every result is checked by the gate before it can be staged.
- **Negative**: a row both sides edited still stops the rebase, now with exit 1 instead of a silently wrong file; the resolver needs the index stages, so a bad resolution that is already committed must still be fixed by hand; the row and tombstone patterns now live in two files (the gate and the resolver) and have to change together.
- **Neutral / follow-ups**: [Research-1383](../research/1383-state-md-three-way-conflict-replay.md) replays the pre-rebase heads of the PRs merged since 2026-09-20 that touched `docs/state.md` through the resolver and compares each result with what landed. `docs/rebase-notes.md` records the invariants a rebase of the resolver must keep.

## References

- req: task brief (2026-09-30): "rewrite scripts/dev/resolve-state-md-conflict.py into a row-level THREE-WAY merge of docs/state.md by bug id" and "both changed differently -> exit non-zero naming the ids and write nothing (design an explicit per-id override flag)".
- req: coordinator (2026-09-30): "Treat each disposition row as a keyed record: key = the bold label in cell 1; merge the id list in cell 2 as a SET three-way".
- [ADR-0165](0165-state-md-bug-tracking.md) (the ledger and its update protocol), [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) and [ADR-1352](1352-rc-phase-shift-plus-one.md) (the disposition table).
- PR #1389 (the resolver this replaces); PRs #1625, #1627 and #1629 (the failures).
