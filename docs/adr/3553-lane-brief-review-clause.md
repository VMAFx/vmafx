<!-- markdownlint-disable MD013 -->
# ADR-3553: Require the read-only and final-message clauses in vmafx review lane briefs

- **Status**: Accepted
- **Date**: 2026-11-15
- **Deciders**: lusoris
- **Tags**: agents, process, ci, fork-local

## Context

Review work is handed to lane jobs started by the lane launcher
(`tools/lanes/agy-run.sh` in the praetor repository). A review job must leave its
worktree as it found it: the job wrapper (`tools/lanes/agy-job.sh`, praetor `e191dd323`,
lines 306-318) compares `git status --porcelain` before and after a review job and
records rc 79 when any path changed, so the job's report is discarded.

On 2026-11-15 two vmafx review jobs ended this way, the reviews of pull requests #3464
and #3470. Both briefs asked the lane to put scratch copies in a `lane-scratch/` folder
and to write `lane-report.md` in the worktree root, so each finished its review and
then failed the guard. The private review template the dispatching session copies from
already says "Write nothing in the worktree" and "return: final message only", but a
brief written by hand or edited after copying loses those lines and nothing stopped it.
The guard is the only defence, and it fires after the lane time has been spent.

The launcher already has a second place to stop such a brief before the job starts:
`brief_checks` in `agy-run.sh` calls `brief_stage.py required <brief> <worktree>`, which
reads `.config/lane-brief.yaml` from the job's worktree. That file may list strings every
brief must contain (`required`) and strings for one stage only (`required_by_stage`,
keyed by `impl`, `fix`, `review`, `audit` or `research`). The stage comes from the
brief's `task:` line, mapped through the router labels. vmafx had no such file, so the
reader answered "none declared" for every brief.

## Decision

Add `.config/lane-brief.yaml` with one `required_by_stage` entry:

```yaml
required_by_stage:
  review:
    - "Write nothing in the worktree"
    - "return: final message only"
```

The launcher now refuses a vmafx review brief that lacks either line, before the job
starts. The two strings are the ones the review template already carries, so a brief
copied from it passes unchanged.

`scripts/ci/tests/test_lane_brief_config.py` covers the file in the `tooling` suite:

- Shape tests (always run): only reader keys, both clauses present in the review list,
  only known stage keys, the reader's limits of 32 entries of up to 200 characters, and
  a top-level list rejected.
- Routing tests (always run): `.github/test-suites.json` lists the config under the
  `tooling` suite's `source_paths`, so a change to the config alone selects the suite
  that holds this test, and an unrelated `.config/` path selects none.
- Replay tests (run when `LANE_BRIEF_STAGE_PY` or `LANE_TOOLS_DIR` points at the reader):
  `brief_stage.py required` against planted briefs in a scratch worktree. A review brief
  without the clauses gives rc 1 and names both lines, a review brief with them gives
  rc 0, an implementation brief gives rc 0, a brief with no `task:` line gives rc 2, and
  the unsafe review brief gives rc 0 again once the review entry is deleted from the
  config.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the review template as the only source | No new file; no launcher change | Both rc 79 briefs had dropped the template's lines; nothing checks a brief against it | The failure this ADR fixes |
| Rely on the rc 79 guard alone | Already in place and exact | Fires after the job ran, so the lane time is lost and the review is redone | One defence, and a late one |
| Default the clause in the praetor launcher for every repository | One rule for the whole fleet | Changes the shared engine, which lands only in its own train; other repositories may want other review wording | Out of this repository's scope; this file can be dropped when such a default lands |
| **Per-repository `required_by_stage` review entry** (chosen) | Refuses the brief before the job starts; uses the reader's existing contract; reversible by deleting the file | Every vmafx lane brief now needs a `task:` line; CI cannot run the replay tests | — |

## Consequences

### Positive

- A review brief that would end in rc 79 is refused before the lane starts, and the
  refusal names the missing lines.
- The rc 79 guard stays as the second defence for a brief that carries the lines but is
  ignored by the lane.

### Negative

- With `required_by_stage` present, the reader must resolve a stage for every brief. A
  vmafx brief without a `task:` line in its first 20 lines is refused with rc 2
  (`cannot resolve brief stage for required_by_stage`), for implementation briefs too.
  The agent brief shape already starts with `task:`, so current briefs pass.
- CI has no praetor checkout, so the nine replay tests skip there with a stated reason.
  The thirteen shape, boundary, routing and reader-lookup tests run everywhere; the host
  replay with the real 3470 and 3464 briefs and a mutation run are in the pull request
  evidence. A `LANE_BRIEF_STAGE_PY` or `LANE_TOOLS_DIR` that names a missing file fails
  the replay tests instead of skipping them.

### Neutral / follow-ups

- The match is a plain substring match, so a brief must keep the exact wording; changing
  the wording means changing this file and the template together.
- If the praetor launcher gains a fleet-wide review default, this file can be removed by
  a superseding ADR.

## References

- Pull requests #3464 and #3470: the two review lane jobs that ended rc 79 on
  2026-11-15.
- `.config/lane-brief.yaml`, `scripts/ci/tests/test_lane_brief_config.py`,
  `.github/test-suites.json` (`tooling` suite).
- praetor `tools/lanes/brief_stage.py` (`required` command, `LANE_BRIEF_KEYS`) and
  `tools/lanes/agy-job.sh` (review guard, rc 79), at praetor `e191dd323`.
- [Agent reference: lane brief requirements](../development/agent-reference.md#lane-brief-requirements).
- Controller go on finding `external-comm-d9f50232c3` (2026-11-15, paraphrased): ship the
  review clause as an own-repository task-branch pull request with a planted brief that
  fails without it.
