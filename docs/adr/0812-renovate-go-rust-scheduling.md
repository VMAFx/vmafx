# ADR-0812: Renovate — Go/Cargo grouping, schedule, and concurrent-PR cap

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `deps`

## Context

The fork's `renovate.json` already had `forkProcessing: "enabled"` (required for Mend to
process fork repos), grouped GitHub Actions and pre-commit bumps, and auto-merged Python
patch updates. Three gaps remained:

1. **Schedule**: The global `schedule` was `"at any time"`, meaning Renovate could open PRs
   at any hour, including business hours and weekends. With `prConcurrentLimit: 12` and the
   Go module graph's many transitive packages this risked flooding the PR queue during active
   work hours.

2. **Go deps not grouped**: `go.mod` had ~60 direct + indirect dependencies. Without a group
   rule each bump became its own PR, overwhelming the human-review queue and burning CI
   minutes on near-identical runs.

3. **Rust/Cargo deps not grouped**: The `Cargo.toml` workspace was scaffolded (ADR-0702,
   ADR-0707) but had no Renovate rule, so future crate bumps would also get individual PRs.

## Decision

We will apply three changes to `renovate.json`:

1. **Global schedule** changed from `"at any time"` to `"before 6am on weekdays"`. Urgent
   vulnerability alerts keep their own `"at any time"` override (already in
   `vulnerabilityAlerts`).

2. **Go deps** are grouped: minor + patch updates travel as one PR auto-merged on early
   Monday; major bumps are individual and require human review (module-path changes and API
   breaks are common for k8s/grpc families).

3. **Cargo deps** follow the same pattern: minor + patch grouped and auto-merged; major
   individual and manual.

4. **`prConcurrentLimit`** reduced from 12 to 10 to keep the queue manageable now that
   the Go group rule will compress many updates into one PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Leave schedule as `"at any time"` | Bumps land fastest | Noisy during work hours; competes with feature PRs for CI slots | Off-hours batching is net cheaper in CI minutes |
| Auto-merge Go _major_ bumps | Fewer queue items | k8s and grpc major bumps break APIs; human sign-off needed | Too risky for production infrastructure deps |
| One group for all Go + Cargo | Maximum compression | Mixed-language bundle obscures what changed | Separate groups give clearer PR titles and blame history |

## Consequences

- **Positive**: Go and Cargo bumps arrive batched as one or two PRs per Monday morning
  rather than dozens spread through the week. Schedule restriction prevents new PRs from
  racing human-authored PRs for CI capacity.
- **Negative**: Minor Go/Cargo updates are delayed up to a week (until the next Monday
  early-morning window). Acceptable given `minimumReleaseAge: "3 days"` already gates
  recency.
- **Neutral**: `vulnerabilityAlerts` already overrides to `"at any time"` with
  `minimumReleaseAge: "0 days"` — security patches are unaffected by the schedule change.

## References

- Memory entry `project_renovate_fork_processing.md` — `forkProcessing: "enabled"` rationale.
- ADR-0604 — ROCm Renovate manager (precedent for off-hours grouping pattern).
- ADR-0605 — dev-image pinned-dep managers (same manual-review pattern for GPU deps).
- `go.mod` — `github.com/VMAFx/vmafx` module with k8s/grpc/prometheus/MCP deps.
- `Cargo.toml` — workspace root with `vmafx-sys` and TAD extractor crates.
- req: "Audit renovate.json. Verify forkProcessing, avoid weekends+business hours, group Go deps, auto-merge minor+patch known-safe, major needs human approval."
