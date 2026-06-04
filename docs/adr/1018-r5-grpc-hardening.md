<!-- markdownlint-disable MD013 -->
# ADR-1018: MCP exec.CommandContext + controller gRPC panic recovery

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `security`, `mcp`, `go`, `grpc`

## Context

Two independent Go correctness defects found by the r5 scan:

**`exec.Command` without context in `cmd/vmafx-mcp/impl.go`** (r5-mcp-streaming):
Three sites used `exec.Command` (no context) instead of `exec.CommandContext`:

- `runVmafScore` (line 224) — the main vmaf subprocess: on MCP client
  disconnection or timeout, the vmaf process continued scoring to completion,
  potentially monopolising CPU/GPU indefinitely.
- `handleEvalModelOnSplit` (line 527) — Python ONNX evaluation: could block
  forever on a hung Python process.
- `handleProbeBackend` (line 788) — GPU driver probe: a KFD ioctl stall
  could freeze the handler goroutine indefinitely.

Additionally, `runVmafScore` accepted no `context.Context` parameter, so the
caller's cancellation signal could not propagate.

**No panic-recovery interceptors in `cmd/vmafx-controller/grpc_server.go`**
(r5-grpc-correctness): `grpc.NewServer` for the controller was constructed
without unary/stream recovery interceptors. A nil-pointer deref or any panic
in `SubmitJob`, `GetJob`, `CancelJob`, `PullWork`, `ReportResult`, or
`StreamJobs` would be unrecovered and kill the process. `cmd/vmafx-server`
already had `recoveryUnaryInterceptor` / `recoveryStreamInterceptor` (ADR-0978)
but the controller was missing them.

## Decision

1. Thread `ctx context.Context` through `runVmafScore` and update all three
   call sites. Replace `exec.Command` with `exec.CommandContext(ctx, ...)` at
   lines 224, 527, and 788.
2. Change `handleProbeBackend` signature from `(_ context.Context, ...)` to
   `(ctx context.Context, ...)` to expose the context to the `CommandContext`
   call.
3. Add `recoveryUnaryInterceptor` and `recoveryStreamInterceptor` to the
   controller's `grpc.NewServer` options, mirroring the server implementation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add time.AfterFunc kill to exec.Command | No signature change | Does not propagate client cancellation; manual timer management | Less idiomatic than ctx |
| Wrap panics in each handler individually | Granular control | Verbose; easy to miss new handlers | Interceptor catches all handlers automatically |

## Consequences

- **Positive**: MCP client cancellation now kills the vmaf subprocess promptly.
  Controller panics are recovered and returned as `codes.Internal` instead of
  crashing the process.
- **Negative**: `runVmafScore` signature changed; callers updated in the same
  commit.
- **Neutral / follow-ups**: The `handleDescribeWorstFrames` path that calls
  `runVmafScore` also benefits automatically.

## References

- r5-mcp-streaming findings: impl.go:224, :527, :788
- r5-grpc-correctness finding: grpc_server.go:418
- ADR-0978: vmafx-server gRPC panic recovery (original implementation mirrored here)
