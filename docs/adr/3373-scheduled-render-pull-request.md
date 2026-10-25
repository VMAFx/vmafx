<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3373: A scheduled workflow opens the render pull request

- **Status**: Accepted
- **Date**: 2026-10-25
- **Deciders**: lusoris
- **Tags**: ci, docs, release, process

## Context

[ADR-3295](3295-render-only-pull-request.md) moved the render of
`CHANGELOG.md`, the ADR index and `docs/rebase-notes.md` into a render-only
pull request per landing batch, and bounded an owed render on master at 24
hours. It named no automation that opens that pull request. The merge train's
render step was written for the direct push the ruleset now refuses, and the
landing script used since then has no render step, so every render pull
request (#2896, #2902, #2928, #2950, #2969) was opened by hand.

Master went red on the bound twice in two days: on 2026-10-23, and on
2026-10-25, when master `de69502de` carried a render owed since `addc88866`
(2026-10-23 23:56 UTC) for 24.9 hours. In both cases nothing ran the render
because nobody started it.

## Decision

1. **Scheduled opener.** `.github/workflows/docs-render-pr.yml` runs every
   hour on master (and on demand). `scripts/ci/check_render_on_master.py
   --open-after-hours 6` decides from master's history alone: `open=true` once
   a render has been owed for 6 hours, with the commit and age it is owed
   since.
2. **One render branch, re-rendered, never hand-merged.**
   `scripts/ci/open-render-pr.sh` runs `make docs-render` on the master tip,
   refuses a render that writes outside the rendered paths, commits the result
   on `chore/docs-render-auto` and force-pushes that branch, opens the pull
   request if none is open and arms auto-merge (squash). A branch whose parent
   is already the master tip is left alone, so a waiting pull request does not
   restart its checks every hour; when master moves, the next run renders on
   the new tip. When master's render is current again (for instance a render
   landed by hand), the open render pull request of this workflow is closed.
3. **Release-bot identity.** The workflow pushes and opens the pull request
   with the release-bot App installation token
   ([ADR-3356](3356-release-token-app-only.md)): a pull request opened with
   `GITHUB_TOKEN` gets no check runs
   ([ADR-1151](1151-vmafx-first-release-1-0-0.md)). The commit is authored by
   the App's bot user and signed off with the same address, so the DCO check
   passes without an exception.
4. **Merges only at CLEAN.** Auto-merge is armed, never forced: the forge
   merges the pull request when every required check passes, as for every
   other pull request. ADR-3295's 24-hour bound and the master-push check are
   unchanged; the 6-hour start leaves 18 hours for checks and merge.

This decision amends ADR-3295 by naming the automation that opens its render
pull request.

## Measured

- `scripts/ci/tests/test_check_render_on_master.py`: 15 cases, all passing.
  New: four decision cases (current render, owed for 2 hours, the planted
  case of a render owed for 7 hours with nothing opened, owed past the bound)
  and two wiring cases (the workflow is scheduled, calls the decision with a
  threshold of at most 12 hours, mints the App token and runs the opener; the
  opener renders, signs off, refuses foreign paths and arms auto-merge).
- Mutations: with the decision forced to `false`, the two opening cases fail;
  with the `schedule` trigger removed from the workflow, the wiring case fails.
- On master at `e1fc7ba3e` (2026-10-25 02:57 CEST) the decision prints
  `open=true`, `owed_since=addc88866199…`, `owed_hours=25.0`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep opening the render pull request by hand | No new workflow | Missed twice in two days; depends on someone remembering | The failure this ADR fixes |
| Render step in the local landing script | Runs right after a batch | Runs only when a session lands; an operator merge or a bot merge leaves the render owed | Does not cover every landing |
| Workflow on every master push | Render follows each landing at once | Every landing re-pushes the render branch and restarts its checks; the render pull request's own merge triggers another run | Churn without benefit; the hourly run with a threshold reaches CLEAN well inside the bound |
| **Hourly workflow after 6 hours owed, App token, auto-merge** (chosen) | Covers every way master moves; one pull request at a time; no bypass | A render waits at least 6 hours; one more scheduled workflow | — |

## Consequences

### Positive

- Master no longer depends on a person to open the render pull request.
- A hand-opened render pull request still works; the workflow closes its own
  pull request when master's render is current.

### Negative

- One more scheduled workflow, at most 15 minutes per hour when a render is
  owed, a few seconds otherwise.
- The render branch is force-pushed by the bot; nobody else commits to it.

### Neutral / follow-ups

- If the App token step fails, the master-push check still turns red after
  24 hours, as before.

## References

- ADR-3295 (render-only pull request), ADR-3356 (release-bot App token),
  ADR-1151 (pull requests need an App token for checks).
- Controller order `vmafx-red-main-de69502de-docs-render-20261025`
  (paraphrased): land the owed render, find why no render ran, fix it, and
  add a case that fails when no render runs within the bound.
