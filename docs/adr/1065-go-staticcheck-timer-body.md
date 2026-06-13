<!-- markdownlint-disable MD013 -->
# ADR-1065: Go staticcheck r10 — poll-loop timer leak and missing body guards

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris automated agent (r10 parallel-push)
- **Tags**: `go`, `security`, `correctness`

## Context

A round-10 staticcheck audit of the Go codebase found three classes of
correctness bugs across four files:

1. **SA1015-analog — `time.After` inside `for`/`select` poll loops**
   (`pkg/storage/http_serve.go:waitForHTTP`,
   `pkg/storage/fuse_mount.go:waitForPath`).  Each loop iteration called
   `time.After(interval)`, allocating a new `*time.Timer` on the heap.  When
   the `ctx.Done()` arm fires first, the timer fires later (after `interval`)
   with no reader — a goroutine-heap timer leak that accumulates across many
   concurrent readiness probes.  `observability.go` already documented the
   correct fix (`time.NewTimer` + `defer Stop()`) and cited ADR-1017; the
   two storage helpers had not received the same treatment.

2. **Missing `http.MaxBytesReader` on the controller score handler**
   (`cmd/vmafx-controller/http_server.go:handleScore`).  The `vmafx-server`
   equivalent already capped the request body at 1 MiB and mapped the overflow
   to HTTP 413.  The controller handler omitted this guard, allowing an
   unauthenticated or low-privilege caller to push an arbitrarily large body
   and consume memory proportional to the body size while the JSON decoder
   reads it.

3. **Missing `ReadTimeout` on both HTTP servers** (controller and server).
   `ReadHeaderTimeout` was set (preventing Slowloris on the header phase), but
   `ReadTimeout` — which covers the body read — was absent.  Combined with the
   missing `MaxBytesReader` on the controller, this left the body-read phase
   unbounded in wall time.  The server already had `MaxBytesReader`; both
   servers now also carry `ReadTimeout: 30 * time.Second`.

## Decision

1. Replace `time.After(interval)` in both poll loops with a
   `time.NewTicker(interval)` created before the loop; `defer ticker.Stop()`
   ensures the ticker is released when the loop exits on any path.

2. Add `http.MaxBytesReader` to `handleScore` in the controller, mirroring the
   1 MiB cap already present in the server.  Map overflow to HTTP 413.

3. Add `ReadTimeout: 30 * time.Second` to both `http.Server` literals.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `time.After` but add `// nolint` | No code change | Suppresses a real timer leak | Timers accumulate during FUSE mount probing; fix is trivial |
| Use `time.Sleep` instead of ticker | Simpler loop | Ignores `ctx.Done()` during sleep; not safe | Context cancellation must be honoured |
| Omit `ReadTimeout` and rely only on `MaxBytesReader` | MaxBytesReader caps data volume | Wall-clock time still unbounded for slow senders | Defence in depth; 30 s is generous for 1 MiB JSON |

## Consequences

- **Positive**: Poll loops no longer accumulate unreachable timers when a
  context cancellation races a poll interval.  The controller score endpoint
  now rejects oversized bodies before reading them into memory.  Both servers
  are hardened against slow-body attacks.
- **Negative**: none — the ticker is a standard Go idiom; `ReadTimeout` does
  not affect normal traffic (a 1 MiB JSON body transfers in milliseconds).
- **Neutral**: The `MaxBytesError` branch in the controller now requires
  `errors.As`, adding the `errors` import.

## References

- ADR-1017 (grpc-dial-backoff) — first instance of `time.NewTimer` + `defer Stop()` in this codebase.
- SA1015 staticcheck rule: "Using `time.Tick` in a short-lived context may cause a memory leak."
- `observability/observability.go` comment: "Use time.NewTimer so the timer is stopped … preventing a goroutine-timer leak. ADR-1017."
