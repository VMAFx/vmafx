<!-- markdownlint-disable MD013 -->

# ADR-1140: Route required CI work by measured impact instead of pre-declared path filters

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `docs`, `fork-local`

## Context

Every required check in this repository decides *whether it applies* before
anything about the change is known. Three workflows that host required
contexts (`libvmaf-build-matrix.yml`, `sanitizers.yml`,
`tests-and-quality-gates.yml`) carry an identical, hand-maintained
`paths-ignore` list; `docs.yml` carries a `paths:` allow-list; the rest
(`lint-and-format.yml`, `security-scans.yml`) run everything on every PR.
ADR-0313 then teaches the aggregator to treat a check that *never appears*
as passing, because a path filter had rejected the whole workflow.

That design hard-codes the routing decision ahead of runtime and has three
costs that have been paid repeatedly this week:

1. **Every PR pays for the whole repository.** A 12-minute
   `Cppcheck (Whole Project)`, a 13-minute `CodeQL (C/C++)`, five Windows /
   Linux build legs and three sanitizer legs run for a two-line `renovate.json`
   or `docs/` change. Seven Renovate PRs landing at once produced 69
   simultaneous runs and starved the one PR that fixed master.
2. **The filters drift from the tree.** The three copies of `paths-ignore`
   must agree with each other and with the aggregator's list of 26 required
   names; when they do not, a check either burns time it should not or —
   worse — a required context silently never registers and the aggregator's
   absent-means-pass rule waves the PR through with no signal that anything
   was skipped.
3. **The "numbers" live in YAML, not in the change.** Whether a doc-only PR
   needs the golden tests is not a property of the workflow file; it is a
   property of the diff. Encoding it as a static ignore-list means every new
   top-level directory, every renamed fixture, every new gate is a place to
   forget.

`lusoris/k8s` solved the same problem on 2026-09-02 (its PR #4964, "route
required checks by impact"): keep every protected context always-starting,
derive a bounded, fail-closed plan from the exact event revisions, and gate
the *work* inside each job on that plan. The user asked for the same here:
stop hard-coding pre-runtime decisions where a runtime measurement exists, so
CI stops blocking on 100 % of the repository for every change.

## Decision

We will add one planner, `scripts/ci/plan-ci-impact.py`, driven by one
declarative map, `.github/ci-impact.json`, and route required work through
it:

- **Required contexts always start.** Workflow-level `paths:` /
  `paths-ignore:` filters are removed from every workflow that hosts a check
  named in `required-aggregator.yml`. The job registers on every non-draft PR
  and every push to `master`; ADR-0313's absent-means-pass rule stays only as
  a backstop for genuinely non-applicable checks (GPU-gated legs).
- **The plan is derived from the event's exact revisions.** For a pull
  request the planner diffs from the *merge-base* of the PR head and base, so
  a PR is judged by what it adds, not by what `master` did in the meantime.
  For a push it diffs the exact `before..head`. It reads Git's NUL-delimited
  `--name-status` stream (renames and copies preserved) and maps paths onto
  named **selectors** (`c_core`, `python`, `ai`, `go`, `rust`, `docs`,
  `shell`, `actions`, `container`) plus derived closures (`golden_harness` =
  C ∪ Python harness, `tiny_ai` = C ∪ AI ∪ Python, `python_lint` = Python ∪
  AI).
- **Fail closed.** The plan collapses to `mode=full` (every selector `true`,
  i.e. today's behaviour) whenever it cannot *prove* the change is scoped: an
  unknown top-level path, any status other than add or modify (delete,
  rename, copy, type or mode change), a change to any CI-authority file
  (the map, the planner, `scripts/ci/**`, the workflows hosting required
  contexts, `.pre-commit-config.yaml`, `Makefile`, `.clang-tidy`, …), a
  missing merge-base, a non-linear push, an over-large diff, or an event the
  planner does not route.
- **Work is gated per step, not per workflow.** Each required job runs the
  planner first, then either executes its real steps (`if:
  steps.impact.outputs.<selector> == 'true'`) or emits a deterministic notice
  explaining which selector was not impacted — and reports `success` either
  way, so the aggregator sees a real conclusion with a real reason.
- **The map is tested against the tree.** `scripts/ci/tests/test_ci_impact.py`
  fails if a top-level directory or file is missing from the map (which would
  force `full` mode and silently defeat the routing), if any required-context
  workflow regains a path filter, or if the planner's fail-closed rules
  regress. It runs on stdlib `unittest` only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Planner + always-starting contexts** (chosen) | One declarative source of truth, merge-base aware, fail-closed by construction, no reliance on absent-means-pass, a doc-only PR runs the cheap gates only | New script + map to maintain; `fetch-depth: 0` on the planner's checkout | — |
| Keep `paths-ignore` lists, just deduplicate them into one reusable workflow input | Smallest diff | Still pre-runtime, still cannot express "C *or* Python harness changed", still relies on absent-means-pass, still no test tying the list to the tree | Keeps every cost in §Context |
| `dorny/paths-filter` per job | Off-the-shelf | Third-party action on every required job; per-job filter lists reintroduce the drift; no fail-closed semantics for renames/deletes/unknown roots | Reintroduces the drift the planner removes |
| Make the aggregator itself compute impact and mark non-impacted required checks as satisfied | No per-workflow edits | Would have the aggregator assert `success` for work that never ran — indistinguishable from a broken trigger, exactly the ADR-0313 failure mode | Hides absence instead of explaining it |
| Reduce the required-check list to the cheap gates | Immediate throughput | Silently drops the build / sanitizer / golden gates from protection; the numerical-correctness contract of the fork depends on them | Not acceptable |

## Consequences

- **Positive**: a `docs/` or `renovate.json` PR runs the sub-minute gates and
  reports `success` on every required context in about a minute instead of
  ~25 minutes of matrix time; the Renovate train no longer starves feature
  PRs; a new top-level directory fails a test instead of silently forcing
  full mode; the three divergent `paths-ignore` copies are gone.
- **Negative**: planner steps need the base commit reachable, so the
  checkouts in the routed jobs move to `fetch-depth: 0` (a few seconds
  each); a non-additive change (rename, delete) intentionally runs
  everything, which is slower than a perfect answer but never wrong.
- **Neutral / follow-ups**: ADR-0313 is *extended*, not replaced — its
  absent-means-pass rule remains for GPU-gated legs that genuinely do not
  apply. Non-required workflows (`go-ci.yml`, `rust-ci.yml`,
  `ffmpeg-integration.yml`, `dev-container-build.yml`) keep their path
  filters for now; migrating them is mechanical and tracked as a
  follow-up. The coverage floors in `scripts/ci/coverage-check.sh` remain
  hard-coded per file; deriving them from a measured baseline is the next
  step in the same direction.

## References

- req: the user asked to take over the CI-unblocking changes from
  `lusoris/k8s` and clarified the intent — stop hard-coding numbers and
  decisions before runtime where a measurement exists, so CI does not block
  on 100 % of the repository for every change (2026-09-02).
- `lusoris/k8s` PR #4964 "ci(ci): route required checks by impact" (merged
  2026-09-02) and #4896 "route manifest validation by impact" — the design
  this ADR ports.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — the aggregator whose
  absent-means-pass rule this ADR reduces to a backstop.
- [ADR-1135](1135-ci-twin-drift-gate.md) — the most recent gate added to the
  required list; runs on every PR because it is sub-second.
