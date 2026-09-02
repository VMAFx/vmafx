**vmafx-controller Phase 4b.1 — job queue, node registry, scheduler** (ADR-0711)

`cmd/vmafx-server` is renamed to `cmd/vmafx-controller` and expanded into the
distributed platform controller.  Binary name changes from `vmafx-server` to
`vmafx-controller`.

- New `VmafxController` gRPC service on the existing port (:50051) alongside the
  retained `VmafxScoring` service.  8 new RPCs:
  - Client API: `SubmitJob`, `GetJob`, `CancelJob`, `StreamJobs`
  - Node API: `RegisterNode`, `Heartbeat`, `PullWork`, `ReportResult`
- SQLite-backed job queue (`cmd/vmafx-controller/queue/`) with crash-recovery
  (running jobs reset to pending on restart).  Pure-Go driver (`modernc.org/sqlite`).
- In-memory node registry (`cmd/vmafx-controller/nodes/`) with 60 s heartbeat timeout.
- FIFO + capability-match scheduler (`cmd/vmafx-controller/scheduler/`).
- New Prometheus metrics: `vmafx_controller_jobs_pending`,
  `vmafx_controller_jobs_running`, `vmafx_controller_nodes_registered`,
  `vmafx_controller_jobs_{submitted,completed,failed,cancelled}_total`.
- `docker/Dockerfile.controller` replaces `Dockerfile.go-server`.
- Helm chart `deploy/helm/vmafx/values.yaml` updated to target `vmafx-controller`.
- SQLite schema: `cmd/vmafx-controller/queue/schema.sql`.
