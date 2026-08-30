# ADR-0359 — ARC self-hosted runner pool: pilot via `ARC_RUNNERS_ENABLED` flag

- Status: Accepted
- Date: 2026-05-09
- Tags: ci, infra, fork-local

## Context

The fork's CI matrix has 50+ required-check legs per PR. On the GitHub
Actions free-tier shared runner pool, deep queue depth (observed 80+
queued jobs across 40+ in-flight PRs on 2026-05-09) blocked the merge
train for 2+ hours — the symptom that prompted [ADR-0358](0358-ci-aggregator-timeout-bump.md).
The owner runs an `arc-runners` scale set
([actions/actions-runner-controller](https://github.com/actions/actions-runner-controller))
in their personal Kubernetes cluster as a registered self-hosted
runner pool on `VMAFx/vmafx`. As of this ADR, the scale set is
registered (visible in `Settings → Actions → Runners → Self-hosted
runners → arc-runners`) but no workflow uses it yet.

GitHub Actions does **not natively support "use self-hosted if
working, else fall back to GitHub-hosted"**. `runs-on:` is a fixed
label selector at workflow-define time. The closest practical
graceful-degradation pattern is a **repo variable that toggles the
selector via expression**: `runs-on: ${{ vars.ARC_RUNNERS_ENABLED ==
'true' && 'arc-runners' || 'ubuntu-latest' }}`.

## Decision

Pilot the ARC pool by routing exactly **one** job — `Cppcheck (Whole
Project)` in `.github/workflows/lint-and-format.yml` — through the
new ternary-expression `runs-on:` selector. Default the variable
`ARC_RUNNERS_ENABLED` to `false` so the migration is opt-in. To
verify ARC is alive: flip the variable to `true` in `Settings →
Secrets and variables → Actions → Variables`, push any change, watch
the Cppcheck job pick up on an `arc-runners` pod. If the ARC pool is
unhealthy the job sits queued indefinitely — observable failure;
flip the variable back to `false` to recover.

After the pilot is green for ≥ 1 day on at least 5 PRs, ramp up via
follow-up PRs to:

1. The 3 sanitizer legs (asan / tsan / msan)
2. The 4 Vulkan + GPU build legs (Vulkan, CUDA, SYCL, SYCL+CUDA)
3. The 2 Windows MSVC legs

Leave fast jobs (lint, deliverables, secrets) on GitHub-hosted —
they don't bottleneck on queue depth and ARC churn isn't free.

## Alternatives considered

- **Always route to `arc-runners` (no fallback)**. Simplest. If ARC
  dies, jobs hang and the train stalls. Rejected: the failure mode
  is too costly during incidents.
- **Workflow-level `workflow_run` chain to detect ARC health
  pre-flight**. Heavyweight; adds 30 s+ to every PR; the pre-flight
  itself can fail. Rejected for the pilot — may revisit if the
  ternary pattern proves unwieldy at scale.
- **Migrate everything to ARC, drop GitHub-hosted entirely**. ARC
  cluster is one-of-one; if it dies the fork is unbuildable.
  Rejected.
- **Commercial runner-orchestrator services (`runs-on.com` etc)**.
  Adds vendor + cost; unjustified for a solo-dev fork.

## Consequences

- One workflow expression added; readable and reversible.
- New repo variable `ARC_RUNNERS_ENABLED` documented in
  [`docs/development/ci-runners.md`](../development/ci-runners.md).
- When ARC is up, Cppcheck takes ~5 min on a k8s pod instead of
  whatever the GitHub queue serves. When ARC is down (variable
  `false`), Cppcheck runs on `ubuntu-latest` exactly as before.
- Future jobs that want the same pattern duplicate the ternary
  expression. After 3 jobs migrate, factor into a reusable
  workflow input or a composite action so each call site stays
  one line.

## References

- User direction 2026-05-09 ("use them if they are working") —
  resolved as the variable-switch pattern documented above.
- [`Settings → Actions → Runners → Self-hosted runners`](https://github.com/VMAFx/vmafx/settings/actions/runners)
  — `arc-runners` scale set registration.
