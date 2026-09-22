<!-- markdownlint-disable MD013 -->

# 1291 — What the silent-revert gate was actually reporting on the zero-warning train

**Date**: 2026-09-22
**Scope**: the 28 findings `scripts/ci/check-silent-revert.py` produced for
`integration/zero-warning-hiss21` against `origin/master` (`371ff5891`).
**Outcome**: 12 were detector defects, 16 were correct detections of reversals
an accepted ADR mandates. [ADR-1291](../adr/1291-silent-revert-declared-reversals.md).

## The 28, derived rather than assumed

Every finding was re-derived from the merge result, not taken from a previous
summary. The breakdown is 23 `reverse-hunk` and 5 `resurrected`; no `rewind`
and no `dropped`.

| Class | Count | Verdict |
| --- | --- | --- |
| `reverse-hunk` on files the merge **deletes outright** | 7 | Detector defect |
| `resurrected` on lines the target **never held** | 4 | Detector defect |
| `reverse-hunk` undoing `c2a3c7e0f`'s dataset-path rename | 16 | Correct — ADR-1277 |
| `resurrected` restoring the ADR-0759 `buf_dev` tier | 1 | Correct — ADR-0759 |

### Defect 1 — a deliberate deletion is a reverse-hunk against whoever wrote the file

`_undone()` treats a **pure-addition** target commit as undone when the merge
removes the lines it added and puts nothing in their place. A file deletion
satisfies that exactly: every line is live, all of them are removed, and
`eff_added` is empty. So `testdata/check_borders.py`, `testdata/compare_a380.py`
and `testdata/scores_sycl_b580_576_mq.json` — the three files `b8888f0d2`
deleted because [ADR-0880](../adr/0880-unused-testdata-debug-scripts-cleanup.md)
retired them and its changelog fragment had already claimed the deletion — each
produced findings against `e704022fb` (which added them) and against `9d55e10c7`
(which added their licence headers). So did the two changelog fragments
`1beb3b8a9` removed when it made the CI gates fail closed.

The repair is a boundary, not a filter: `reverse-hunk` skips a path the merge
deletes. Nothing is lost, because `dropped` already answers the only question
worth asking about a deletion — whether the branch asked for it. For a
declared deletion every line is in the branch's own removals and `dropped` is
quiet; for an undeclared one none of them are and it fires. A fixture test
pins both directions.

### Defect 2 — `resurrected` never checked the half of its definition that matters

The docstring reads "text the target had deleted, coming back". The code only
asked whether the line was absent from the target. A merge commit that writes
its own conflict resolution produces lines that are in the merge, absent from
the target, and absent from every non-merge branch commit (no single commit's
diff contains them) — which satisfied the old test completely.

`837c81678`, the merge that reconciled the HISS-01 zero-`goto` unwind refactor
with the restored `buf_dev` tier, authored 55 such lines in
`core/src/feature/hip/integer_adm_hip.c`, 10 in that directory's `AGENTS.md`, 8
in `docs/rebase-notes.md`, and one `docs/state.md` row — the row that documents
the silent revert this gate caught. A fifth came from `.pre-commit-config.yaml`,
where `bc84a866a` wrote the line without `pyproject\.toml` and a later
resolution inserted it.

`git log -S` over `origin/master` confirms the discriminator: none of those
lines has ever existed on the target. The repair requires the line to appear in
the set of lines the target lost over the history window.

### The 16 that are right — ADR-1277

`c2a3c7e0f` (#1067, 2026-05-16) rewrote every dataset path from `.corpus/` to
the retired numbered workspace directory.
[ADR-1277](../adr/1277-workingdir-contract-cleanup.md) (Accepted, 2026-09-20)
names that "the incorrect first migration attempt" and splits local data into
`.workingdir/` for machine-local state and `.corpus/` for datasets;
`scripts/ci/check-local-data-contract.sh` is the required gate that keeps the
retired root out. So the branch really does undo `c2a3c7e0f`, and the gate is
right to say so.

How *completely* it undoes it is the useful measurement. Recomputing the full
`live ∪ restored` evidence set for all sixteen findings — not the six-line
excerpt the report prints — gives **zero** lines that do not contain
`.workingdir2/` or `.corpus/`. The reversal is the path text and nothing else
of that mega-squash. That is what makes an `evidence` regex a real bound rather
than decoration.

### The 1 that is right — ADR-0759

`(void)hipFree(s->buf_dev);` and `s->buf_dev = NULL;` are
[ADR-0759](../adr/0759-hip-adm-buffer-by-pointer.md), landed in `31a51afb2`
(#101) and silently reverted by `92ea978a4` (#102) — the pair ADR-1284 was
written from. Master still carries the reverted state, so restoring them *is*
text the target lost coming back, and the repaired detector still reports it.
The branch's own `6811ef63d` writes both lines; the finding survives only
because the later unwind reconciliation re-indents a second copy of each, so
the merge holds one more occurrence at four-space indentation than any single
commit added.

## Why not the declaration ADR-1284 already provides

`reverts: #N` in the PR body would have passed the branch. It is per-PR and
total: it turns *every* finding in the run into a notice. This run is the
counter-example — the gate had already caught a real loss in `28fba56e5`, where
a merge dropped two `docs/state.md` rows still live on master. A single body
line would have exempted that too.

## Verification

- 28 findings → 0 blocking, with both allowlist entries reporting as notices that name their ADR.
- The gate re-run against `28fba56e5` with the repairs and the allowlist in place still exits **1** on the `dropped` finding for `docs/state.md`, and the sixteen ADR-1277 findings in that same run become notices. The class the gate exists for is intact.
- `scripts/ci/tests/test_check_silent_revert.py`: 22 cases pass; 5 of them fail against the unrepaired gate, so each repair is exercised rather than asserted.
