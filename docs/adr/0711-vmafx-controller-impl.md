<!-- markdownlint-disable MD013 -->
# ADR-0711: vmafx-controller Phase 4b.1 — Job Queue, Node Registry, and Scheduler

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `architecture`, `go`, `controller`, `grpc`, `sqlite`, `job-queue`, `node-registry`, `scheduler`, `phase4b`, `fork-local`

## Context

Phase 4b (ADR-0709) pivoted VMAFX from a single-binary scoring tool to a distributed
video-quality platform with a controller/node split.  The Phase 4a `vmafx-server`
(ADR-0703) shipped a gRPC + HTTP service with direct libvmaf scoring.  Phase 4b.1 is
the first expansion of that binary into a cluster controller:

- Rename `cmd/vmafx-server/` to `cmd/vmafx-controller/`.
- Keep the existing `VmafxScoring` gRPC service and HTTP endpoints (backward-compatible
  for Phase 4a clients).
- Add a new `VmafxController` gRPC service exposing a job queue and node API.
- Persist jobs in SQLite (pure-Go driver, no external process needed) for crash recovery.
- Track live `vmafx-node` instances in an in-memory registry with heartbeat-based eviction.
- Implement a capability-aware work scheduler (FIFO + backend match) for round-trip
  job dispatch.
- Add controller-specific Prometheus gauges and counters.
- Update `docker/Dockerfile.controller` (formerly `Dockerfile.go-server`) and the Helm
  chart `deploy/helm/vmafx/` to target the renamed binary.

## Decision

The Phase 4b.1 scope is as follows.

### Rename

`cmd/vmafx-server/` becomes `cmd/vmafx-controller/` via `git mv`.  The binary name
changes from `vmafx-server` to `vmafx-controller`.  References in the Helm chart,
Dockerfile, and log messages are updated accordingly.

### gRPC API expansion

A new service `VmafxController` is added in
`cmd/vmafx-controller/proto/controller.proto`.  The existing `VmafxScoring` service
is retained unmodified.

**Client API** (CLI, MCP, future Web UI):

- `SubmitJob` — enqueue a scoring job; returns a job ID.
- `GetJob` — retrieve job state by ID.
- `CancelJob` — cancel a pending or running job.
- `StreamJobs` — server-streaming snapshot of jobs (Phase 4b.1: snapshot; persistent
  subscription in Phase 4b.2).

**Node API** (vmafx-node workers):

- `RegisterNode` — announce node capabilities on startup.
- `Heartbeat` — keepalive every 10 s.
- `PullWork` — pull the next matching PENDING job.
- `ReportResult` — report terminal (or partial) outcome.

Total new gRPC RPCs: **8** (4 client + 4 node).  Pre-existing scoring RPCs: **2**.
Grand total on the port: **10**.

### Job queue

`cmd/vmafx-controller/queue/` implements `Queue` (interface) and `SQLiteQueue`
(concrete type).  Backing store: `modernc.org/sqlite` (pure-Go; no cgo required for
the controller outside the libvmaf scoring path).  Schema: single `jobs` table with
a `(status, created_at)` index for scheduler hot path.  On controller restart, any
`RUNNING` job is reset to `PENDING` (assigned node is gone).

### Node registry

`cmd/vmafx-controller/nodes/` implements an in-memory registry keyed by
controller-assigned `node_id`.  Nodes are evicted after 60 s without a heartbeat.
No SQLite persistence: nodes re-register on startup.

### Scheduler

`cmd/vmafx-controller/scheduler/` is a thin coordination layer over queue + registry.
Policy: oldest PENDING job whose `backend` requirement is satisfied by the requesting
node's capabilities.  Round-robin fairness among equal-capability nodes is deferred to
Phase 4b.2.

### Observability

New Prometheus gauges and counters added to `pkg/observability`:

- `vmafx_controller_jobs_pending` (gauge)
- `vmafx_controller_jobs_running` (gauge)
- `vmafx_controller_nodes_registered` (gauge)
- `vmafx_controller_jobs_submitted_total` (counter)
- `vmafx_controller_jobs_completed_total` (counter)
- `vmafx_controller_jobs_failed_total` (counter)
- `vmafx_controller_jobs_cancelled_total` (counter)

## Alternatives considered

### PostgreSQL or Redis for the job queue

PostgreSQL or Redis would provide richer querying, pub/sub for streaming job updates,
and horizontal scalability.  Rejected for Phase 4b.1 because:

- A single controller instance is the expected deployment for initial rollout.
- SQLite eliminates an external process dependency, simplifying the dev and test
  environment.
- The `Queue` interface is designed to allow a PostgreSQL/Redis backend swap without
  changing callers.

### Stateful job push (streaming subscription)

A long-lived `StreamJobs` subscription (watching for new job events) would be more
efficient than polling for clients.  Deferred to Phase 4b.2 because it requires an
event fan-out mechanism (channel broadcast, Redis pub/sub, or in-memory). The
snapshot model is sufficient for Phase 4b.1 CLI + MCP integration.

### Separate proto file vs inline in the existing vmafx.proto

Adding controller messages to `proto/vmafx.proto` would reduce the number of files
but conflate two distinct API surfaces with different versioning concerns.
A separate `cmd/vmafx-controller/proto/controller.proto` keeps the scoring API and
the controller API independently versioned.

## References

- Parent: ADR-0709 (VMAFX Phase 4b distributed platform).
- Origin: ADR-0703 (vmafx-server Phase 4a gRPC + HTTP service).
- `req`: "rename to vmafx-controller; add job queue (postgres or redis for v1; could
  move to k8s Job CRDs later), node registry, scheduler" — paraphrased from Phase 4b
  planning doc (project_vmafx_phase4b_distributed_platform.md).
