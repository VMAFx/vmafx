<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1049: Exponential backoff for vmafx-node online-feedback drainLoop

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `go`, `grpc`, `node`, `bug`

## Context

`online_feedback.go` used a fixed `feedbackRetryInterval = 10s` constant.
When the Python sidecar is unavailable for extended periods (e.g. rolling
restarts, OOMKills, node eviction), the drainer retries at the same rate
indefinitely, generating steady log noise and unnecessary dial attempts at
a constant 10-second cadence. This is the pattern described in the R9 bug
hunt report (finding: `online_feedback.go:60`).

## Decision

Replace the fixed `feedbackRetryInterval` constant with an exponential
backoff: initial interval `feedbackRetryBase = 2s`, doubling on each
consecutive failure, capped at `feedbackRetryMax = 2m`. A successful
connection resets the backoff to `feedbackRetryBase`.

The cap at 2 minutes is chosen so that:

- A sidecar that restarts within a rolling-update window (~60s) is
  reconnected within at most one missed interval.
- Log noise during extended outages (e.g. node eviction) is bounded to
  one message every 2 minutes rather than one every 10 seconds.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fixed interval (status quo) | Simple | Steady log noise during extended outages; wastes connection slots | Removed |
| Full `grpc.WaitForReady` with service-config retry policy | gRPC-native | Requires migrating from Unix socket to gRPC transport; out of scope | Future work |
| Jitter (full jitter / equal jitter) | Prevents thundering herd | Thundering herd not a concern for a single-node sidecar connection | Not needed |

## Consequences

- **Positive**: Log noise during sidecar outages is bounded; connection
  attempt frequency drops after the first few failures.
- **Negative**: After a 2-minute outage, the first reconnect attempt is
  delayed by up to 2 minutes. Acceptable given the fire-and-forget
  design of the feedback client.
- **Neutral / follow-ups**: The `drainLoop` comment is updated to document
  the backoff semantics.

## References

- R9 bug hunt report finding: `online_feedback.go:60` (2026-06-04).
- `cmd/vmafx-node/online_feedback.go`.
