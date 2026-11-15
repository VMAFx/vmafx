<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3552: Run the render pull request workflow on every master push

- **Status**: Accepted
- **Date**: 2026-11-15
- **Deciders**: lusoris
- **Tags**: ci, docs, release, process

## Context

`.github/workflows/docs-render-pr.yml` ([ADR-3373](3373-scheduled-render-pull-request.md))
triggered only on `schedule` (`cron: "23 * * * *"`) and `workflow_dispatch`.
Between `2026-11-13 00:00` and `2026-11-15 01:16 UTC`, GitHub Actions fired
only 9 scheduled runs out of 49 expected hourly slots, with gaps of up to 8
hours (issue #3462).

The open render pull request (#3453) went `BEHIND` when `master` moved at
`23:40 UTC`. No scheduled run fired in time to re-render the branch on the new
`master` tip, GitHub auto-merge does not merge a `BEHIND` pull request, and the
`master` `Docs` job went red on the 24-hour render bound of
[ADR-3295](3295-render-only-pull-request.md) (run `43654367833`:
`render owed since 15673bb4e106 ... 24.7 h ago; the bound is 24 h`).

## Decision

1. **Trigger on every `master` push; keep the hourly schedule as fallback.**
   `.github/workflows/docs-render-pr.yml` adds `push` with `branches: [master]`
   (and no path, tag or branch-ignore filter) as the first trigger under `on:`.
   `schedule` (`cron: "23 * * * *"`) remains as the hourly fallback when
   `master` receives no pushes after a batch passes the 6-hour threshold, and
   `workflow_dispatch` remains unchanged.
2. **Safety without opener or decision changes.** `permissions:`,
   `concurrency:`, `env:`, the job, its steps,
   `scripts/ci/check_render_on_master.py` and `scripts/ci/open-render-pr.sh`
   stay unchanged:
   - While a render has been owed for less than `OPEN_AFTER_HOURS` (`6`), the
     `decide` step prints `open=false` and a non-empty `owed_since`, so a
     `master` push in that window finishes right after `decide` without minting
     the release-bot App token or running `open-render-pr.sh`.
   - When a push lands the render pull request itself, `master` becomes current
     (`owed_since` empty); `open-render-pr.sh` looks up open pull requests for
     `chore/docs-render-auto` and closes nothing because none is open.
   - If `chore/docs-render-auto` already has the `master` tip as its parent,
     `open-render-pr.sh:66` exits cleanly without force-pushing or restarting
     checks.
   - The workflow concurrency block (`group: docs-render-pr`,
     `cancel-in-progress: false`) keeps at most one active run plus one pending
     run across `master` pushes and schedules (`SERIALISED` in
     `scripts/ci/tests/test_master_concurrency_contract.py`).
3. **Amendment to ADR-3373.** This decision amends
   [ADR-3373](3373-scheduled-render-pull-request.md) decision 1 and reverses
   its rejection of "Workflow on every master push". That rejection assumed the
   hourly scheduled run fires reliably, which measured GitHub Actions execution
   does not. The churn ADR-3373 named — re-rendering and restarting the render
   pull request's checks when `master` advances — happens only while a render
   is already due (`>= 6` hours owed), and then it is necessary: a `BEHIND`
   render pull request never auto-merges, so the next run would re-render
   anyway.

## Measured

- `scripts/ci/tests/test_check_render_on_master.py`: 16 cases passing,
  including `RenderOnMasterWiringTest.test_render_pull_request_runs_on_master_push`
  (asserts `push.branches == ["master"]`, no branch other than `master`, no
  `paths`, `paths-ignore`, `tags` or `branches-ignore` filter under `push`,
  `schedule` retained with one `cron` entry, and `workflow_dispatch` retained).
- Mutations on scratch workflow copies (`build/lane-3462/`):
  - (a) `push` trigger removed -> `test_render_pull_request_runs_on_master_push` fails;
  - (b) `push.branches` set to `[main]` -> `test_render_pull_request_runs_on_master_push` fails;
  - (c) `push` given `paths: [docs/**]` -> `test_render_pull_request_runs_on_master_push` fails;
  - (d) `schedule` removed (`push` only) -> `test_render_pull_request_runs_on_a_schedule` fails (and `test_render_pull_request_runs_on_master_push` fails).
- Scheduled-run delivery on `VMAFx/vmafx` (reproducer:
  `gh run list -R VMAFx/vmafx --workflow docs-render-pr.yml --limit 60 --json createdAt,event`):
  9 scheduled runs fired between `2026-11-13 00:00` and `2026-11-15 01:16 UTC`
  against 49 expected hourly runs, with gaps up to 8 hours; #3453 stayed
  `BEHIND` after `23:40 UTC` and `master` `Docs` failed at `24.7 h` owed
  (`15673bb4e106`, run `43654367833`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep hourly schedule only | No workflow run on `master` pushes | Fired 9 of 49 expected runs with gaps up to 8 hours; left #3453 `BEHIND` past the 24-hour bound | The failure this ADR fixes |
| More frequent cron schedule | Keeps trigger decoupled from `push` | Uses the same GitHub Actions cron scheduler that drops runs; adds idle polling when `master` is quiet | Same scheduler drops runs |
| `workflow_run` after master `Docs` reports the owed warning | Fires only once `Docs` logs the warning | Fires later than `push` (after the `Docs` job finishes) and only once the warning is printed | Slower than `push` and indirect |
| **Push trigger on `master` plus hourly schedule fallback** (chosen) | Runs as soon as `master` advances; hourly schedule still covers quiet periods after the 6-hour threshold; no opener code change | One short `decide`-only run per `master` push inside the 6-hour window; re-renders on each landing while due | — |

## Consequences

### Positive

- Every push to `master` starts `docs-render-pr.yml`, refreshing a due render
  pull request onto the new `master` tip without waiting on dropped cron runs.
- The hourly `schedule` remains as a fallback when no push lands after an owed
  render crosses `OPEN_AFTER_HOURS`.

### Negative

- One short run per `master` push: it ends after `decide` while a render has
  been owed for less than 6 hours; when `master`'s render is current, it also
  mints the release-bot token and `open-render-pr.sh` finds no render pull
  request to close.
- While a render is due, each `master` landing re-renders `chore/docs-render-auto`
  and restarts the render pull request's required checks.

### Neutral / follow-ups

- Revisit trigger: if the render pull request is still found `BEHIND` past the
  24-hour bound with `push` runs present, the duration of the render pull
  request's checks is the cause (follow-up: shorten the render pull request's
  checks).

## References

- Issue #3462 (`docs-render-pr.yml` dropped scheduled runs left #3453 `BEHIND`
  past the 24-hour bound).
- [ADR-3373](3373-scheduled-render-pull-request.md) (scheduled render pull
  request — amended here), [ADR-3295](3295-render-only-pull-request.md)
  (render-only pull request and 24-hour bound),
  [ADR-1151](1151-vmafx-first-release-1-0-0.md) (App token required for check
  runs on bot-opened pull requests).
- Controller order `vmafx-bug-wave-20261113` (2026-11-15, issue #3462,
  paraphrased): trigger `docs-render-pr.yml` on `master` pushes with the hourly
  schedule kept as fallback, and cover the trigger block with positive,
  negative and boundary wiring tests.
