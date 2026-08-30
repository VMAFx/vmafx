<!-- markdownlint-disable MD060 -->
# ADR-0448: Active upstream monitoring (no silent "wait" deferrals)

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, governance, upstream-sync, deferral, fork-local

## Context

Per the 2026-05-15 user direction: **"we don't defer anything; the
only allowed shape is 'blocked behind external X' and even then we
actively search for gaps around X."** The fork's `docs/state.md`
"Deferred (waiting on external trigger)" section had several rows
that nominally tracked an upstream PR with comments like "Scheduled
remote agent re-runs weekly until merged" — but the actual scheduling
lived in an external system that nobody could verify in-tree. Result:
rows aged silently, the scheduled check ran or didn't run with no
audit trail, and "deferred" in practice became "forgotten".

The first row to feel this was Netflix#955 (`i4_adm_cm` rounding):
the row had been marked "blocked on Netflix PR #1494 merging" since
2026-04-25; the most recent in-tree last-checked date was 2026-05-09.
Without an in-tree mechanism, there's no way to differentiate
"someone checked this week and the upstream PR is still OPEN" from
"nobody checked because the external scheduler died".

## Decision

Every "deferred (waiting on external trigger)" row in `docs/state.md`
that names a specific upstream artefact (PR, issue, release tag) must
have a matching in-tree GitHub Actions workflow that:

1. Runs on a recurring schedule (default: weekly).
2. Polls the named upstream artefact via `gh` against the upstream
   repo.
3. If the trigger condition fires (e.g. upstream PR merges), opens a
   fork-side tracking issue with a label like `upstream-merged` so
   the deferral can be actively closed.
4. If the trigger has NOT fired, no-op — the workflow's run history
   in the Actions tab doubles as the audit log.

Workflows live under `.github/workflows/upstream-<short-name>-watcher.yml`
with a one-line `name:` that names the deferral being watched.

The first concrete instance is
`.github/workflows/upstream-netflix-955-watcher.yml`, which polls
`Netflix/vmaf#1494` weekly (Sunday 06:00 UTC).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **In-tree workflow per deferral (chosen)** | Audit trail in Actions tab; auto-opens fork-side tracking issue when trigger fires; no external dependency | Requires writing one workflow per deferral row | Chosen — workflow shapes are short and copy-pasteable |
| **Single multi-deferral cron + state-file lookup** | Less workflow proliferation | Adds a config file to maintain; harder to read at-a-glance which deferral is being watched | Rejected — explicit-per-deferral is clearer for reviewers |
| **External scheduled agent ("remote agent re-runs weekly")** | What we had before | No in-tree audit trail; aged silently; fork can't verify the schedule is actually firing | Rejected — exactly the failure mode this ADR is fixing |
| **Drop the deferral; port the upstream fix immediately, accept golden-test breakage** | Closes the row | Violates CLAUDE.md §8 (Netflix golden assertions are bit-exact ground truth) and `feedback_no_test_weakening` | Rejected — the deferral exists for a real CI-gating reason |
| **Manual "check upstream every release" per-release-process step** | Lowest infrastructure | Easy to forget; ties deferral resolution to release cadence (which can be slow) | Rejected — automation is cheap, mistakes are not |

## Consequences

### Positive

- Every external-triggered deferral becomes actively watched, in-tree,
  with an audit trail.
- The Actions tab becomes the canonical "what's the upstream status
  of our deferred items" dashboard.
- No more "the external agent died and we didn't notice" failure mode.

### Negative

- One workflow file per deferral row. The fork currently has ≤5
  external-trigger deferrals, so the proliferation is bounded.
- Workflows consume GitHub Actions minutes. At weekly cadence and
  ~5 s per run, the cost is negligible (~5 s × 5 deferrals × 52 weeks
  ≈ 22 minutes/year).

### Neutral

- Existing `docs/state.md` "Deferred" rows that reference external
  triggers should grow a "watched by `<workflow-file>`" cross-link in
  a follow-up sweep PR.

## References

- User direction (2026-05-15, Slack): "we don't defer anything"; the
  only allowed shape is "blocked behind external X" with active
  monitoring.
- `docs/state.md` Deferred section — rows this ADR governs.
- ADR-0155 — `i4_adm_cm` deferral that motivated the first watcher.
- `.github/workflows/upstream-netflix-955-watcher.yml` — first
  instance.
