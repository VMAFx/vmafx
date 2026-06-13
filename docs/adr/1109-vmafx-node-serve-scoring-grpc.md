<!-- markdownlint-disable MD013 MD060 -->
# ADR-1109: vmafx-node `Serve()` registers the VmafxScoring gRPC service

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: lusoris, Claude (Phase-4b RC build-out)
- **Tags**: go, node, grpc, scoring, streaming, phase4b, fork-local

## Context

`cmd/vmafx-node/server/server.go::Serve()` shipped as a Phase-4b.4 scaffold: it
bound a TCP port, logged readiness, and blocked on `ctx.Done()` **without
registering any gRPC service**. A node started this way accepted connections but
answered no RPCs, so it could not actually do work — the worker side of the
distributed platform (ADR-0709 / ADR-0713) had a hole where its service surface
should be.

Two service-surface shapes are defensible for the node, and the proto is the
source of truth on which exist:

- `proto/vmafx.proto` defines exactly one scoring service, **`VmafxScoring`**
  (`Score`, `ScoreStream`, `Health`).
- `cmd/vmafx-controller/proto/controller.proto` defines `VmafxController`, whose
  Node API (`RegisterNode` / `Heartbeat` / `PullWork` / `ReportResult`) the node
  consumes **as a client** against the controller — it is not a service the node
  hosts.

ADR-0713 describes the node's controller-pull loop (`PullWork → Execute →
ReportResult`) as a *client* role. That loop is orthogonal to what the node
*serves*. The decision here is only about the service the node hosts on its own
listen port.

## Decision

`vmafx-node`'s `Serve()` registers the **`VmafxScoring`** service — `Score`
(unary, file-path), `ScoreStream` (bidirectional, in-memory per-frame, ADR-0933),
and `Health`. This makes a node a directly-dispatchable scoring endpoint (push
model), reusing the same `pkg/libvmaf` engine the standalone `vmafx-server`
uses, with a graceful-shutdown path (GracefulStop + hard-stop fallback) that
respects the node's 30 s SIGTERM budget from ADR-0713.

The node's scoring engine is optional: when `Config.Scorer` is nil the node still
serves `Health` (so liveness probes and the Phase-4b.4 smoke test pass) and
returns `codes.FailedPrecondition` from the scoring RPCs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Register `VmafxScoring` (chosen)** | The only scoring contract the proto defines; reuses the proven `vmafx-server` handler shape and the shared `pkg/libvmaf` engine; gives the node a dispatchable endpoint immediately; matches the existing `server.go` doc ("accepts the controller's job-dispatch calls"). | The node now has two roles (served scoring + future controller-pull client). | Smallest, contract-faithful change that turns the stub into a working service without inventing a new proto. |
| Wire the controller-pull loop (`PullWork → Execute → ReportResult`) instead | Matches ADR-0713's lifecycle narrative directly. | Much larger change to `main.go` (controller dial, heartbeat goroutine, retry/backoff); needs a live controller; does not give the node a *served* surface at all, leaving `Serve()` still empty of services. | Out of scope for "register the service the node exposes"; the pull loop is a separate, larger work item that can coexist with a served `VmafxScoring`. |
| Define a new node-only gRPC service in a new proto | Bespoke node API surface. | The proto is the source of truth and defines no such service; inventing one duplicates `VmafxScoring` for no added meaning today. | Premature; would fork the scoring contract. |
| Register `Health` only | Minimal; keeps the smoke test green. | Node still cannot score over gRPC — the hole stays open. | Does not deliver the worker surface the platform needs. |

## Consequences

- **Positive**: a node is now a working push-model scoring endpoint; the
  ScoreStream streaming surface (ADR-0933) is available on every node, not just
  the standalone server. Graceful shutdown is consistent with `vmafx-server`.
- **Positive**: `pkg/libvmaf` is the single scoring engine across server and
  node — no duplicated cgo path.
- **Negative**: the node `server` package now depends on cgo (`pkg/libvmaf`),
  which it already did transitively via the executor; the `Scorer` field is
  optional so Health-only deployments still work.
- **Neutral / follow-up**: the controller-pull worker loop (ADR-0713) remains a
  separate work item. A node can serve `VmafxScoring` *and* later run the pull
  loop; the two are independent.

## References

- See [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) for the
  controller/node/operator platform context.
- See [ADR-0713](0713-vmafx-node-impl.md) for the node's controller-pull client
  lifecycle (the role orthogonal to what the node serves).
- See [ADR-0933](0933-grpc-streaming-multi-frame-scoring.md) for the
  `ScoreStream` contract the node now serves.
- See [ADR-0703](0703-vmafx-server-go-grpc.md) for the `vmafx-server` handler
  shape this mirrors.
- Source: per user direction — Phase-4b RC build-out task brief, 2026-06-13
  ("register the appropriate gRPC service(s) the node is meant to expose").
