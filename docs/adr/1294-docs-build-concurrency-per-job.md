<!-- markdownlint-disable MD013 MD060 -->
# ADR-1294: Scope the docs build's concurrency group to its ref, not to Pages

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: VMAFx maintainers
- **Tags**: `ci`, `docs`

## Context

`.github/workflows/docs.yml` carried GitHub's Pages starter-workflow boilerplate
at the top level:

```yaml
concurrency:
  group: "pages"
  cancel-in-progress: false
```

A workflow-level `concurrency` applies to every job in the file. The group name
is a constant — it carries no `github.ref` — so every open pull request and every
push to `master` competes for one slot across the whole repository. A concurrency
group holds at most one *pending* run, so when a third arrives the one already
waiting is cancelled rather than queued. `cancel-in-progress: false` does not
prevent that; it only stops a running job from being torn down by its successor.

That group is correct for the `deploy` job, because GitHub Pages accepts one
deployment at a time. It is wrong for `build`, which runs `make
docs-fragments-check` and `mkdocs build --strict` on every pull request and
deploys nothing.

Measured on 2026-09-22: run `35739609377`, the docs build for PR #1518 at
`160ddfc3d`, started 14:20 and was cancelled at 14:30:20 inside
`upload-pages-artifact` by run `35740184612` — a Renovate Docker-digest bump on
an unrelated branch that started at 14:25 and succeeded. Neither the fragment
freshness check nor the strict MkDocs build completed for that PR, and the check
reported `cancelled`.

The docs-site build is not in the required-checks aggregator (`Docs` there is a
different job, in `lint-and-format.yml`), so this did not block a merge. It is
still a gate that did not run being recorded as something other than a failure,
which is the shape HISS-18 forbids: "a gate that did not run is never reported as
passing". The branch this lands on exists to remove exactly that class of hole —
it has already removed a `|| true` that hid five months of truncated coverage
runs.

## Decision

Delete the workflow-level `concurrency` block and give each job its own.

- `build` gets `group: docs-build-${{ github.workflow }}-${{ github.ref }}` with
  `cancel-in-progress: true`. One docs build per branch, superseded only by a
  newer push to that same branch, which is the same policy the CI and
  build-matrix workflows already use.
- `deploy` keeps `group: "pages"` with `cancel-in-progress: false`, so a
  deployment already writing to Pages is never torn down mid-write. It runs on
  `push` to `master` only (`if: github.event_name == 'push'`), so it is not a
  source of pull-request contention.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `${{ github.ref }}` to the single workflow-level group | One-line change | Also splits the `deploy` group, so two branches could deploy to Pages concurrently — the one thing the global group exists to prevent | Fixes the build at the cost of the deployment |
| `cancel-in-progress: true` at workflow level | Stops the pending-slot eviction | Makes it worse for `deploy`: an unrelated branch could cancel a deployment mid-write | Wrong direction |
| Skip the `build` job on pull requests | Removes the contention entirely | Removes the only per-PR run of `mkdocs build --strict`, so a broken link or a missing nav entry would reach `master` unchecked | Deletes the check instead of fixing it |
| Make the docs build a required context so a cancellation blocks the merge | Cancellation stops being silent | Does not stop the cancellation; it converts an unreliable check into an unreliable blocker, and every PR would need a re-run | Treats the symptom |
| Per-job concurrency: ref-scoped `build`, global `deploy` | Each job gets the policy it needs; no cross-PR contention; Pages still serialised | Slightly more YAML than one top-level block | Chosen |

## Consequences

- **Positive**: a pull request's docs build no longer depends on what other
  branches are doing; `mkdocs build --strict` and `make docs-fragments-check`
  actually run for every PR that touches docs; a stale docs build is superseded
  by the next push to its own branch instead of lingering.
- **Negative**: none identified. Pages deployments remain serialised by the
  unchanged `pages` group on `deploy`.
- **Neutral / follow-ups**: the required-checks aggregator lists `Docs`
  (`lint-and-format.yml`) and not this workflow's `build`. Whether the docs-site
  build should also be required is a separate question this ADR does not settle;
  it is worth asking once the check is reliable enough to be one.

## References

- req: "And get it green, what do you mean by not relevant? Are we fixing or
  destroying" — per user direction, a check that did not run is not written off
  because it does not gate.
- [ADR-1140](1140-ci-impact-planner.md) — the impact planner that
  already skips this job when docs are untouched.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — how required contexts are
  resolved, and why absence is treated as path-filter-skipped.
