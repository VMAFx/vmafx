# `docs/state.md` gates and conflict resolver

Every PR that closes a bug, opens a bug or rules a Netflix upstream report
not-affecting-the-fork updates [`docs/state.md`](../state.md) in the same PR
(rule 15 of the [agent hard rules](agent-hard-rules.md),
[ADR-0165](../adr/0165-state-md-bug-tracking.md)). This page covers the three
tools that keep that file honest: the touch check that requires the edit, the
row-hygiene check that keeps rows in the right section, and the resolver that
merges the file during a rebase.

| You are... | Read |
| --- | --- |
| Opening a bug-fix PR | [Bug-status hygiene gate](#bug-status-hygiene-gate-adr-0165-adr-0334) |
| Rebasing a branch that touches `docs/state.md` | [Resolving a rebase conflict](#resolving-a-statemd-rebase-conflict) |
| Reading a failure from the row check | [Row hygiene](#row-hygiene) |

## Resolving a state.md rebase conflict

`docs/state.md` conflicts on almost every rebase of a branch that touches it,
and unlike the other append-only bookkeeping files it is deliberately not in
the `merge=union` list in [`.gitattributes`](../../.gitattributes). Its rows
move between the "Open bugs" and "Recently closed" sections, so a union merge
would duplicate the row and leave a closed bug reading as open forever.

When `git rebase` stops on it, run the resolver and continue:

```bash
# mid-rebase, with docs/state.md conflicted
python3 scripts/dev/resolve-state-md-conflict.py docs/state.md
git add docs/state.md && git rebase --continue
```

Exit 0 means the file is written and
[`scripts/ci/check-state-md-rows.sh`](../../scripts/ci/check-state-md-rows.sh)
already passed on it; the resolver runs the gate itself. The file is written
with LF line endings on every platform.

### What the resolver does

[`scripts/dev/resolve-state-md-conflict.py`](../../scripts/dev/resolve-state-md-conflict.py)
ignores the conflict markers. It reads the three versions git keeps in the
index for a conflicted path (`git show :1:docs/state.md` is the merge base,
`:2:` is *ours*, `:3:` is *theirs*) and merges them three-way:

| Line | Keyed by | Rule |
| --- | --- | --- |
| Bug row (first cell opens with an id: `**T-ID**`, `T-ID`, `**T7-16**`, `Netflix#NNN`, `**Netflix/vmaf#NNN**`) | bug id | Its state is its text plus its `##` section. Same on both sides: kept. Changed on one side only: that side wins, so an edit, a move to "Recently closed", or a deletion carries over. Changed differently on both: conflict. |
| Move tombstone (`<!-- T-ID moved to Recently closed ... -->`) | bug id | Same as a bug row. |
| Disposition row under "First-release phase classification" (first cell a bold label such as `**RC8 benchmarks, profiling and tuning**`, second cell a `<br>` list of ids) | bold label | Both sides changed it: the id list merges as a set (ours, plus the ids theirs added, minus the ids either side removed; ours' order, theirs' additions after) and every other cell three-way by text. Rows repeating a label on one side are folded into one first, and the resolver says so. |
| Anything else (headings, prose, `_Updated` lines) | line text | Line-level three-way. Lines both sides add at the same place are all kept, theirs after ours. A non-blank line both sides added is kept once, which is what a branch stacked on an already squash-merged PR needs. Lines either side deleted go, even where the two deletions overlap. |

The row and tombstone shapes are the ones `check-state-md-rows.sh` recognises,
so the two agree on what a row is.

Placement keeps ours' order; a line only theirs has goes after its nearest
theirs neighbour that is still present in the same section. Two consequences
are deliberate:

- your branch's new `_Updated` line and its newly closed rows land below the
  ones master added since you branched (move them up by hand if you want the
  ledger newest-first);
- a reorder of existing rows within one section on your branch is not carried
  over.

During a rebase *ours* is master plus the branch commits already replayed and
*theirs* is the commit being replayed. That is why neither side may simply
win. "Ours wins" keeps master's stale Open copy of a row the branch closes. It
also keeps an earlier branch commit's text of a row that a later commit of the
same branch rewrote. The resolver before this one did exactly that, three
times on 2026-09-30
([ADR-1383](../adr/1383-state-md-three-way-conflict-resolver.md)).

### When it stops

When both sides changed the same row, tombstone, disposition cell or line
differently, the resolver writes nothing, names each one with its base, ours
and theirs text, and exits 1. Decide which side is right and rerun with one
`--take` per reported name:

```bash
python3 scripts/dev/resolve-state-md-conflict.py docs/state.md \
    --take T-FOO-2026-09-30=theirs \
    --take 'RC8 benchmarks, profiling and tuning=ours' \
    --take line:468=theirs
```

A bug id takes that side's row and tombstone; a disposition label takes that
side's whole row; `line:N` is the handle the report prints for a conflicting
plain line (N is its line number in the merge-base version). A `--take` that
names nothing in the file is an error.

| Exit | Meaning |
| --- | --- |
| 0 | Written; the row gate passes. |
| 1 | Conflicts; nothing written. |
| 2 | Bad usage, or the path has no unmerged index stages (not mid-rebase, or already `git add`-ed). |
| 3 | Written, but the row gate rejects the result or a bug id sits in two disposition rows. Fix the file before `git add`. |

The resolver only works while git still holds the three stages. After a bad
resolution has been committed, fix `docs/state.md` by hand; the gate in CI
still catches duplicated and misfiled rows.

### Tests

[`scripts/dev/test-resolve-state-md-conflict.py`](../../scripts/dev/test-resolve-state-md-conflict.py)
builds throwaway repositories and drives the resolver through real
`git rebase` conflicts:

- a branch closing a bug, and a later commit rewriting an earlier one's row;
- both sides adding `_Updated` lines and closed rows;
- a branch stacked on a PR master already squash-merged;
- a row closed on master while the branch edited it;
- one-side and overlapping deletions, tombstones, disposition rows edited on
  both sides, and the refusals.

Every resolved file must pass the row gate.

```bash
python3 scripts/dev/test-resolve-state-md-conflict.py -v
```

CI runs it, and the gate's own self-test, in the required `Tooling Tests`
job, which runs every test of the tooling suite once
([test suites](test-suites.md), ADR-1568). The `Release Script Contract` job
of [`.github/workflows/rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml)
runs the gate on the live `docs/state.md`.

## Row hygiene

A duplicate is not the only way a closed bug reads as open. A row filed under
`## Open bugs` whose own rightmost cell already says `closed` or `fixed` reads
as an open bug forever without any duplicate being involved: the PR appended
its row to the section it happened to be reading instead of moving it, or a
rebase dropped the move hunk and kept the status edit. 24 of the 62 rows under
`## Open bugs` were in that state on 2026-09-21.

`scripts/ci/check-state-md-rows.sh` reads the section heading each row sits
under together with the status token in its status cell and requires them to
agree:

| Status token | Allowed under |
| --- | --- |
| `closed`, `fixed`, `resolved`, `done` | `## Recently closed` only |
| `open` | `## Open bugs` only |

How the token is read:

- the status cell is the column a table header calls `Status`, or the last
  non-empty cell when no header names one;
- the token read is the word that opens that cell, so `fixed (PR #1425)` is
  judged exactly like `fixed`;
- rows that lead with no status token (a verification date, a branch name,
  prose) make no status claim and are not judged.

The fix is always to move the row, never to rewrite its status to match where
it landed.

### Tombstones

An explicit move tombstone closes the common no-status case. If a comment under
`## Open bugs` says an id "moved to Recently closed", the same id may not still
have a table row in Open bugs. This is checked independently of table status
columns: a bookkeeping change once added the PTQ row's tombstone while leaving
the stale Open row directly above it, and all status-token checks reported
clean. The valid result is the tombstone in Open bugs plus the single
authoritative row under `## Recently closed`.

### Code spans and brackets close on their line

Every line of `docs/state.md` outside a fenced block must pair each backtick
run with a later run of the same length on that line (a code span) and close
every `[` that is not inside a code span. The gate names the line and column of
the first run or bracket that does not close.

Most of the ledger is one long paragraph of rows, so one stray backtick
re-pairs every code span after it in the rendered page, and a `[` left outside
a span makes the GFM autolink-literal parser behind the Documentation
Governance job walk back to it from every later URL candidate. One row with a
debugger frame name in single backticks took the lint of this file from 4 s to
over two minutes, past the job's 120 s budget per lint process
(`T-STATE-MD-UNPAIRED-CODE-SPAN-LINT-TIMEOUT-2026-10-06`). To fix a hit:

- write a backtick that belongs to the text inside a longer run, for example
  ``` `` mod`close `` ``` for a debugger frame name that joins module and
  function with a backtick;
- put a `[` that has no `]` into a code span, or escape it as `\[`.

### Limits of the check

The check is a floor on this class of drift, not a proof of its absence. It
reads one cell per row, so a status it does not recognise (buried mid-cell, in a
column that is neither the last nor headed `Status`, or spelled outside the
vocabulary above) is passed over in silence and the file still reports clean.

Of the two ways the check can be silently disabled it fails closed on one: if
any row claims a status that belongs to a section and that section's heading is
missing, the gate errors instead of passing over rows that have quietly become
ungated. The other, an unrecognised status cell, is uncovered, and is the
likelier of the two, since it takes a single row edit rather than a heading
rename.

## Bug-status hygiene gate (ADR-0165, ADR-0334)

The `state-md-touch-check` job in
[`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml) (check
name `docs/state.md Gate`) fails a bug-shaped PR that does not touch
`docs/state.md`. It is backed by the single-purpose script
[`scripts/ci/state-md-touch-check.sh`](../../scripts/ci/state-md-touch-check.sh)
([ADR-0334](../adr/0334-state-md-touch-check-ci-gate.md)); until that ADR the
rule was reviewer-enforced.

### When the gate fires

Any of these:

- the PR title carries a Conventional-Commit `fix:` or `fix(scope):` prefix;
- the PR title contains the bare token `bug` (word boundary, so `debug` does
  not fire);
- the PR title or body contains a `closes`, `fixes` or `resolves` `#N` GitHub
  issue close keyword (case-insensitive);
- the PR body has the `## Bug-status hygiene` template section with the
  `docs/state.md` checkbox left unchecked.

### When it clears

Either:

1. The pull request's own diff, from the merge base of `BASE_SHA` and
   `HEAD_SHA` to `HEAD_SHA` (what master changed since the branch forked
   does not count), includes
   [`docs/state.md`](../state.md) (the row landed in the appropriate section:
   Open, Recently closed, Confirmed not-affected or Deferred) and none of the
   inserted lines carry a placeholder PR or commit reference
   ([below](#placeholder-references)); or
2. The PR description contains `no state delta: REASON`, where REASON is any
   non-empty token that is not the literal placeholder `REASON`. Use this for
   pure `feat`, `refactor` and `infra` PRs that genuinely have no bug-status
   impact. The pull request template keeps its example of the line inside an
   HTML comment, which the gate strips, so an unedited template opts nothing
   out; a placeholder left anywhere else does not void a real opt-out.

### Placeholder references

Touching `docs/state.md` is necessary but not sufficient (ADR-0334 status
update 2026-05-09). PR #541's row audit found that the dominant staleness
pattern is post-merge backfill drift: closing PRs write `this PR` as the
closer-PR placeholder, the merge happens, and the placeholder never gets
rewritten to the merged numeric references. The gate therefore also rejects any
inserted line in `docs/state.md` containing:

| Placeholder | Why |
| --- | --- |
| `this PR` | post-merge backfill drift (most common) |
| `this commit` | same drift mode for SHA-shaped refs |
| `TBD` | obvious fill-it-in-later marker |
| `<PR>` | template placeholder |
| `#NNN` | template placeholder (real refs are digits) |

The canonical accept forms, explicitly not matched, are `PR #N` (any positive
integer) and ``commit `<sha>` `` (the SHA wrapped in backticks). For an
in-flight PR whose number is not yet final, either:

1. land the row with a placeholder, then push a follow-up commit that rewrites
   it to `PR #<number>` after `gh pr create` returns the number; or
2. use `PR #<this-pr-number>` once GitHub has assigned it (the number is known
   the moment `gh pr create` exits).

### Run it locally

The dry-run mirrors the
[`deliverables-check.sh`](../../scripts/ci/deliverables-check.sh) pattern:

```bash
PR_TITLE="fix: foo segfault" \
PR_BODY="$(gh pr view 999 --json body -q .body)" \
  bash scripts/ci/state-md-touch-check.sh
```

Or pipe the body on stdin if `gh` is not on `PATH`:

```bash
gh pr view 999 --json body -q .body \
  | PR_TITLE="fix: foo segfault" bash scripts/ci/state-md-touch-check.sh
```

The companion fixture script
[`scripts/ci/test-state-md-touch-check.sh`](../../scripts/ci/test-state-md-touch-check.sh)
exercises the gate against 22 cases (5 primary, 3 regression, 10
placeholder-ref, 4 bodies built from the pull request template). The Tooling
Tests job runs it; run it after touching either script or the template:

```bash
bash scripts/ci/test-state-md-touch-check.sh
```
