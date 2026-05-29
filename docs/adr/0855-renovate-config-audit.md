# ADR-0855: Renovate config audit — schedule, rate-limits, and missing manager rules

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: ci, dependencies, renovate, go, rust, docker, helm, fork-local

## Context

A routine audit of `renovate.json` against the live repository state revealed four
categories of drift:

1. **Schedule / rate-limits**: `schedule: ["at any time"]` plus `prHourlyLimit: 0`
   and `prConcurrentLimit: 12` allow Renovate to open an unbounded number of PRs at
   any time of day. With 23 required CI checks per PR and a strict one-PR-in-flight
   policy (per `feedback_one_pr_in_flight_strict.md`), this is operationally unsafe.
   An in-progress branch (`chore/renovate-schedule-and-concurrency`) had already
   drafted a fix but was never PR'd.

2. **Missing Cargo rules**: The repository has a Rust workspace
   (`bindings/rust/vmafx-sys`, `core/src/feature/rust/tad`) with `bindgen` and
   `cc` build-time deps. Renovate's `config:best-practices` preset enables the
   `cargo` manager automatically, but without explicit package rules, patch/minor/major
   updates are all treated identically (manual review, no grouping), producing noisy
   individual PRs.

3. **Missing Go module rules**: `go.mod` declares ~60 direct+indirect deps spanning
   k8s, gRPC, Prometheus, Cobra, MCP SDK, and SQLite. Without rules, each module
   version bump gets its own PR.

4. **Missing Dockerfile and Helm rules**: The repo has several `FROM`-pinned Dockerfiles
   (`Dockerfile`, `docker/Dockerfile.production-gpu`, `dev/Containerfile`) and a Helm
   chart with a `prometheus-pushgateway` subchart. Neither the `dockerfile` nor the
   `helmv3` manager had policy rules.

## Decision

Apply the following changes to `renovate.json`:

- Set `schedule` to `["after 6am on Tuesday", "before 6am on Wednesday"]`, `prHourlyLimit`
  to `4`, and `prConcurrentLimit` to `5`.  This matches the pending draft branch and
  keeps the weekly update window predictable.
- Add per-rule schedules on the GH Actions auto-merge and pre-commit auto-merge rules so
  they fire within the same Tuesday window (no Monday-morning PR floods).
- Add explicit package rules for `gomod` (patch auto-merge, minor/major manual).
- Add explicit package rules for `cargo` (patch auto-merge, minor manual).
- Add explicit package rules for `dockerfile` (digest auto-merge, tag bumps manual with
  `dev-image` label).
- Add explicit package rules for `helmv3` (all bumps manual with `helm` label).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `at any time` + `prConcurrentLimit: 12` | No change needed | Violates one-PR-in-flight policy; can flood the queue | Rejected — operational risk |
| Rely solely on `config:best-practices` for Cargo/Go | No extra config | Produces one PR per dep bump with no grouping; noisy | Rejected — excessive noise |
| Separate ADR per manager class | Fine-grained history | Adds four more ADRs for a single config pass | Rejected — over-engineering; one audit = one ADR |

## Consequences

- **Positive**: Renovate PRs arrive predictably on Tuesdays; patch bumps for Go/Rust/Python
  auto-merge after CI passes; GPU-adjacent Dockerfiles remain gated for human review.
- **Negative**: Non-security dependency updates will be delayed up to a week if they land
  on a Wednesday.
- **Neutral / follow-ups**: The pending branch `chore/renovate-schedule-and-concurrency`
  is now superseded; it should be closed/deleted after this PR merges.

## References

- Memory note: `project_renovate_fork_processing.md` — `forkProcessing: "enabled"` required.
- Memory note: `feedback_one_pr_in_flight_strict.md` — strict one PR at a time.
- Source: req (user instruction: "Audit renovate.json for correctness + drift")
