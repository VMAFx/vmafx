<!-- markdownlint-disable MD013 MD060 -->
# vmafx-controller gRPC service

`vmafx-controller` is the distributed platform controller for VMAFX Phase 4b.  It is a single Go binary
that exposes VMAF scoring and job orchestration over both gRPC and HTTP/JSON.

This page covers the gRPC interface. See [http-transport.md](../mcp/http-transport.md) for the
HTTP endpoints (`/healthz`, `/readyz`, `/metrics`, `/v1/score`).

The controller exposes **two gRPC services** on the same port:

| Service | Purpose |
|---|---|
| `VmafxScoring` | Direct scoring (retained from Phase 4a, ADR-0703) |
| `VmafxController` | Job queue + node API (Phase 4b.1, ADR-0711) |

## Quick start

```bash
# Local dev (requires core/build-cpu to exist)
go run ./cmd/vmafx-controller \
    --vmaf-binary core/build-cpu/tools/vmaf \
    --model-dir   model/ \
    --port        8080 \
    --grpc-port   50051 \
    --db          /tmp/vmafx-controller.db

# Docker
docker build -f docker/Dockerfile.controller -t vmafx-controller:dev .
docker run --rm \
    -e VMAFX_VMAF_BINARY=/usr/local/bin/vmaf \
    -e VMAFX_MODEL_DIR=/usr/local/share/vmafx/model \
    -e VMAFX_DB_PATH=/data/vmafx-controller.db \
    -p 8080:8080 -p 50051:50051 \
    vmafx-controller:dev
```

## Configuration

All settings accept CLI flags and 12-factor environment variables.
CLI flags take precedence over environment variables.

| Flag | Env var | Default | Description |
|---|---|---|---|
| `--port` | `VMAFX_PORT` | `8080` | HTTP listen port |
| `--grpc-port` | `VMAFX_GRPC_PORT` | `50051` | gRPC listen port |
| `--log-level` | `VMAFX_LOG_LEVEL` | `INFO` | slog level (DEBUG/INFO/WARN/ERROR) |
| `--vmaf-binary` | `VMAFX_VMAF_BINARY` | _(PATH lookup)_ | Path to the `vmaf` CLI binary |
| `--model-dir` | `VMAFX_MODEL_DIR` | _(none)_ | Directory containing VMAF `.json` model files |
| `--db` | `VMAFX_DB_PATH` | `vmafx-controller.db` | Path to the SQLite job-persistence database |

## VmafxScoring service (direct scoring)

Retained from Phase 4a for backward compatibility.  Clients that already talk to
`vmafx-server` continue to work against `vmafx-controller` without changes.

```protobuf
service VmafxScoring {
  rpc Score(ScoreRequest)   returns (ScoreResponse);
  rpc Health(HealthRequest) returns (HealthResponse);
}
```

Proto source: `proto/vmafx.proto`.  Generated stubs: `gen/go/vmafxv1/`.

### Example: direct score

```bash
grpcurl -plaintext \
    -d '{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","model":"vmaf_v0.6.1"}' \
    localhost:50051 vmafx.v1.VmafxScoring/Score
```

## VmafxController service (job queue + node API)

Phase 4b.1 distributed orchestration surface.  Proto source:
`cmd/vmafx-controller/proto/controller.proto`.  Generated stubs: `gen/go/controller/`.

### Client API

Used by the CLI, MCP server, and future Web UI to submit and track jobs.

```protobuf
service VmafxController {
  rpc SubmitJob(SubmitJobRequest)     returns (SubmitJobResponse);
  rpc GetJob(GetJobRequest)           returns (Job);
  rpc CancelJob(CancelJobRequest)     returns (CancelJobResponse);
  rpc StreamJobs(StreamJobsRequest)   returns (stream Job);  // snapshot in Phase 4b.1
  ...
}
```

#### Submit a job

```bash
grpcurl -plaintext \
    -d '{"scoring":{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","backend":"cuda"}}' \
    localhost:50051 vmafx.controller.v1.VmafxController/SubmitJob
# → {"jobId": "550e8400-e29b-41d4-a716-446655440000"}
```

#### Poll job status

```bash
grpcurl -plaintext \
    -d '{"jobId":"550e8400-e29b-41d4-a716-446655440000"}' \
    localhost:50051 vmafx.controller.v1.VmafxController/GetJob
# → {"id":"...","status":"COMPLETED","scoring":{...},"assignedNode":"node-abc"}
```

### Node API

Used by `vmafx-node` worker processes to pull and report work.

```protobuf
service VmafxController {
  ...
  rpc RegisterNode(RegisterNodeRequest) returns (RegisterNodeResponse);
  rpc Heartbeat(HeartbeatRequest)       returns (HeartbeatResponse);
  rpc PullWork(PullWorkRequest)         returns (PullWorkResponse);
  rpc ReportResult(ReportResultRequest) returns (ReportResultResponse);
}
```

Node lifecycle:

1. On startup, the node calls `RegisterNode` with its capability (GPU vendor,
   available backends, concurrency slots).  The controller returns a `node_id`
   and a `session_token`.
2. The node calls `Heartbeat` every ~10 s with the `node_id` and `session_token`.
   A node that misses heartbeats for 60 s is evicted; its in-flight jobs return to
   `PENDING`.
3. When the node has capacity, it calls `PullWork`.  The controller assigns the
   oldest `PENDING` job whose `backend` requirement matches the node's capabilities.
4. After the job completes (or fails), the node calls `ReportResult` with `final=true`.

### Job lifecycle

```text
PENDING --> RUNNING --> COMPLETED
                    \-> FAILED
PENDING --> CANCELLED
RUNNING --> CANCELLED
```

### Backend capability matching

A job's `scoring.backend` field specifies which backend the job requires
(e.g. `"cuda"`, `"sycl"`, `"cpu"`).  If empty, any node can accept the job.
A node must list the required backend in its `capability.backends` to receive the job.

## Job persistence

The controller persists jobs in a SQLite database at `--db` / `VMAFX_DB_PATH`.
On controller restart:

- `PENDING` jobs are reloaded and re-queued in submission order.
- `RUNNING` jobs are reset to `PENDING` (their assigned nodes are gone).
- `COMPLETED`, `FAILED`, and `CANCELLED` jobs are retained for audit.

Schema: `cmd/vmafx-controller/queue/schema.sql`.

## Prometheus metrics

`/metrics` exposes the following in Prometheus exposition format.

| Metric | Type | Description |
|---|---|---|
| `vmafx_controller_score_requests_total` | Counter | Direct Score requests (HTTP + gRPC VmafxScoring) |
| `vmafx_controller_score_errors_total` | Counter | Direct Score requests that returned an error |
| `vmafx_controller_score_duration_seconds` | Histogram | Direct scoring latency |
| `vmafx_controller_health_requests_total` | Counter | Health / `/healthz` calls |
| `vmafx_controller_ready_requests_total` | Counter | `/readyz` calls |
| `vmafx_controller_jobs_pending` | Gauge | Current number of PENDING jobs |
| `vmafx_controller_jobs_running` | Gauge | Current number of RUNNING jobs |
| `vmafx_controller_nodes_registered` | Gauge | Current live node registrations |
| `vmafx_controller_jobs_submitted_total` | Counter | Jobs submitted via SubmitJob RPC |
| `vmafx_controller_jobs_completed_total` | Counter | Jobs completed successfully |
| `vmafx_controller_jobs_failed_total` | Counter | Jobs that ended in failure |
| `vmafx_controller_jobs_cancelled_total` | Counter | Jobs cancelled |

## Graceful shutdown

The controller listens for `SIGTERM` and `SIGINT`. On receipt it:

1. Stops accepting new connections.
2. Waits up to 30 seconds for in-flight requests to drain.
3. Exits with code 0.

Jobs remain in the SQLite database; the next controller instance reloads them.

## Further reading

- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — Phase 4b.1 decision record.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella architecture.
- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — Phase 4a origin (vmafx-server).
- [HTTP transport docs](../mcp/http-transport.md) — `/healthz`, `/readyz`, `/metrics`, `/v1/score`.
- [k8s deployment guide](../development/k8s-deployment.md) — Helm chart configuration.
