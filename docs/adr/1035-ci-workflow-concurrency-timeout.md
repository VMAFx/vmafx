<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1035: CI workflow concurrency guards and job timeouts

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `ci`, `security`, `supply-chain`

## Context

Five CI workflows lacked `concurrency:` blocks: `nightly.yml`, `nightly-bisect.yml`,
`supply-chain.yml`, and `release-please.yml`. Without a `concurrency:` group, a manual
`workflow_dispatch` can trigger a second run while a scheduled run is in flight,
wasting runner minutes and producing ambiguous results (duplicate provenance attestations
for `supply-chain.yml`, duplicate release PRs for `release-please.yml`).

Three workflow jobs also lacked `timeout-minutes`: the two Rust jobs
(`rust-vmafx-sys`, `cargo-deny`) and the Go job (`go-ci`), plus the Scorecard
analysis job. Without timeouts these jobs consume the GitHub Actions 6-hour default
on any hang (e.g., cgo linker waiting on a missing libvmaf, or a Scorecard API call
stalling indefinitely).

Additionally, `e2e-k8s.yml` used mutable version tags (`@v3`, `@v4`, `@v6`, `@v2`) for
four Docker/artifact actions instead of the immutable SHA pins required by the repo's
supply-chain policy. This creates a supply-chain substitution risk where a tag is
transparently moved to a malicious commit.

## Decision

1. Add `concurrency:` blocks to `nightly.yml`, `nightly-bisect.yml`, and
   `release-please.yml` with `cancel-in-progress: true` (safe to cancel an in-flight
   benchmark or release-PR check in favour of the newer run).
2. Add `concurrency:` to `supply-chain.yml` with `cancel-in-progress: false`
   (cancelling a signing job mid-way produces a partial attestation; serialise instead).
3. Add `timeout-minutes: 30` to `rust-vmafx-sys` and `cargo-deny`, `timeout-minutes: 20`
   to `go-ci`, and `timeout-minutes: 20` to scorecard `analysis`.
4. Add `timeout-minutes: 10` to `release-please` job.
5. Pin `docker/setup-buildx-action`, `actions/cache`, `docker/build-push-action`, and
   `actions/download-artifact` in `e2e-k8s.yml` to the same immutable SHAs already used
   in other workflow files. `EnricoMi/publish-unit-test-result-action@v2` is deferred
   to Renovate for SHA resolution.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave as-is | No diff | Waste of runner minutes, duplicate signing runs, supply-chain risk | Unacceptable |
| Remove `workflow_dispatch` from affected workflows | Eliminates the race | Removes the ability to manually re-run | Too heavy-handed |

## Consequences

- **Positive**: No duplicate signing runs, no 6-hour hung jobs, reduced supply-chain
  attack surface for Docker-related actions.
- **Negative**: A manually dispatched nightly run will cancel the in-flight scheduled
  run; operators should check whether the cron run has already started before dispatching.
- **Neutral / follow-ups**: Renovate should pick up `EnricoMi/publish-unit-test-result-action`
  for SHA resolution on its next scheduled run.

## References

- r7 review findings: [r7-ci-workflow-yaml] nightly, bisect, supply-chain, release-please concurrency; rust-ci, go-ci, scorecard timeouts; e2e mutable pins
