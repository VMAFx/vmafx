<!-- markdownlint-disable MD013 -->
# ADR-1017: Go operator controller resource-allocation fixes

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `security`, `k8s`, `go`, `operator`

## Context

Two classes of resource-management bugs found by the r5-scheduler-timer scan in
`cmd/vmafx-operator`:

**Per-call `http.Client` allocation** (`vmafxnode_controller.go:129`,
`vmafxmodeltraining_controller.go:143`): `probeHealthz` and `pollTrainerStatus`
create a fresh `&http.Client{Timeout: ...}` on every Reconcile call when
`r.HTTPClient == nil`. A new client bypasses Go's `net/http` keep-alive
connection pool (each call creates a new TCP connection and TLS handshake) and
prevents the OS from reusing ephemeral ports efficiently. At `nodeProbeInterval
= 30 s` per VmafxNode object with N objects in flight, this can exhaust local
port space on busy clusters.

**`grpc.WithBlock()` in `getRemoteJob`** (`vmafxjob_controller.go:145`): the
blocking option freezes the controller-runtime reconciler goroutine for up to
`grpcDialTimeout = 5 s` while waiting for the TCP handshake. A non-blocking
dial returns immediately; the first RPC naturally drives the connection to
`READY`. The `ctx` passed to `GetJob` at line 153 was also the root reconciler
context (no deadline), which would let the RPC block indefinitely.

## Decision

1. In `SetupWithManager` for both `VmafxNodeReconciler` and
   `VmafxModelTrainingReconciler`: initialise `r.HTTPClient` once when it is
   nil (production path). Existing test injection (where `HTTPClient` is set
   before `SetupWithManager`) is unaffected.
2. Remove `grpc.WithBlock()` from the `DialContext` call in `getRemoteJob`.
3. Pass `dialCtx` (which carries the `grpcDialTimeout` deadline) to `GetJob`
   instead of the root `ctx`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Cache a shared `*grpc.ClientConn` on `VmafxJobReconciler` | No new TCP dial per Reconcile | Connection lifecycle management in controllers is complex; requires reconnect on error | Deferred to a future PR; per-call overhead is acceptable given Reconcile rate |
| Keep `http.Client` per-call but add a global default | Avoids mutating reconciler struct | Same problem — still creates clients in the hot path | No benefit over field init |

## Consequences

- **Positive**: `probeHealthz` and `pollTrainerStatus` now reuse a shared
  `http.Client` across all Reconcile calls, enabling connection pooling.
  `getRemoteJob` no longer blocks the goroutine during dial.
- **Negative**: None; both changes are semantically equivalent for correctness.
- **Neutral**: Test code that constructs reconcilers without calling
  `SetupWithManager` continues to work via the nil fallback in the helper
  functions.

## References

- r5-scheduler-timer findings: vmafxjob_controller.go:141, :153;
  vmafxnode_controller.go:129; vmafxmodeltraining_controller.go:143
- ADR-0786: vmafx-operator Stage 2 (parent)
