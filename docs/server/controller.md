<!-- markdownlint-disable MD013 MD060 -->
# vmafx-controller gRPC service

`vmafx-controller` is the distributed platform controller for VMAFX Phase 4b.
It is a single Go binary that exposes VMAF scoring and job orchestration over
both gRPC (default `:9090`) and HTTP/JSON (default `:8080`). Configuration is
by `VMAFX_*` environment variables only.

This page covers the gRPC interface. The HTTP routes are `GET /healthz`,
`GET /readyz`, `GET /metrics` and `POST /v1/score`
(`cmd/vmafx-controller/main.go`); their request and response shapes match the
[vmafx-server REST adapter](rest.md). Authentication is in [auth.md](auth.md).

The controller exposes **two gRPC services** on the same port:

| Service | Purpose |
| --- | --- |
| `VmafxScoring` | Direct scoring (retained from Phase 4a, ADR-0703) |
| `VmafxController` | Job queue + node API (Phase 4b.1, ADR-0711) |

## Quick start

1. Run the controller locally. It needs a `vmaf` binary, for example from
   `core/build-cpu`.

    ```bash
    VMAFX_VMAF_BINARY=core/build-cpu/tools/vmaf \
    VMAFX_MODEL_DIR=model/ \
    VMAFX_HTTP_ADDR=:8080 \
    VMAFX_GRPC_LISTEN=:9090 \
    VMAFX_DB_PATH=/tmp/vmafx-controller.db \
    VMAFX_SCORING_ROOTS=/media \
    go run ./cmd/vmafx-controller
    ```

    `VMAFX_SCORING_ROOTS` names the inputs callers may score; without it
    every input is refused ([scoring roots](auth.md#scoring-roots)).

2. Or build and run the container image.

    ```bash
    docker build -f docker/Dockerfile.controller -t vmafx-controller:dev .
    docker run --rm \
        -e VMAFX_VMAF_BINARY=/usr/local/bin/vmaf \
        -e VMAFX_MODEL_DIR=/usr/local/share/vmafx/model \
        -e VMAFX_DB_PATH=/data/vmafx-controller.db \
        -p 8080:8080 -p 9090:9090 \
        vmafx-controller:dev
    ```

## Configuration

All settings are environment variables (12-factor); the controller has no CLI
flags beyond `--version` since ADR-1119. Listen addresses are full addresses
(`:8080`), not bare ports.

<!-- BEGIN GENERATED: vmafx-api environment vmafx-controller (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:8080` | `controller.httpPort` | HTTP listen address of `/healthz`, `/readyz`, `/metrics` and `POST /v1/score`, a full address. |
| `VMAFX_HTTP_TIMEOUTS_READ` | `http.timeouts.read` | duration | `30s` |  | Deadline for reading a whole request; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_HEADER` | `http.timeouts.header` | duration | `5s` |  | Deadline for reading the request headers (slow-client guard); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` | duration | `60s` |  | Deadline for writing a response; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_IDLE` | `http.timeouts.idle` | duration | `120s` |  | Keep-alive idle timeout; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_SHUTDOWN` | `http.timeouts.shutdown` | duration | `30s` |  | Drain time of the HTTP server at shutdown; `0` keeps the framework default. |
| `VMAFX_HTTP_LIMITS_HEADER` | `http.limits.header` | bytes | `1048576` |  | Largest request header block; `0` keeps the default. |
| `VMAFX_HTTP_LIMITS_BODY` | `http.limits.body` | bytes | `10485760` |  | Largest request body; `0` keeps the default. `VMAFX_HTTP_LIMITS_UNLIMITED` removes the cap. |
| `VMAFX_HTTP_LIMITS_UNLIMITED` | `http.limits.unlimited` | bool | `false` |  | Serve request bodies of any size; `VMAFX_HTTP_LIMITS_BODY` is then ignored. |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:9090` | `controller.grpcPort` | gRPC listen address of `VmafxScoring` and `VmafxController`, a full address. |
| `VMAFX_GRPC_TLS` | `grpc.tls` | bool | `false` |  | Serve gRPC over TLS; needs `VMAFX_GRPC_CERT_FILE` and `VMAFX_GRPC_KEY_FILE`. |
| `VMAFX_GRPC_CERT_FILE` | `grpc.cert_file` | path | _(unset)_ |  | PEM certificate of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_KEY_FILE` | `grpc.key_file` | path | _(unset)_ |  | PEM private key of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_MAX_RECV_SIZE` | `grpc.max_recv_size` | bytes | `4194304` |  | Largest gRPC message received; `0` keeps the gRPC default. |
| `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.max_send_size` | bytes | `4194304` |  | Largest gRPC message sent; `0` keeps the gRPC default. |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | `vmaf` on `PATH` |  | Path of the `vmaf` CLI behind the scorer; a missing binary stops the program at startup. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(unset)_ |  | Directory of the VMAF `.json` models the scorer loads. |
| `VMAFX_STORE_BACKEND` | `store.backend` | string | `sqlite` | set by the chart | Where jobs and node sessions live: `sqlite` or `postgres` ([job persistence](controller.md#job-persistence)); anything else stops the controller. |
| `VMAFX_DB_PATH` | `db.path` | path | `vmafx/vmafx-controller.db` under the user state directory | set by the chart | SQLite job database (`sqlite`). Unset, the controller uses `$XDG_STATE_HOME` (else `~/.local/state`) on Linux and the BSDs and the user configuration directory on macOS and Windows; it never writes to the working directory. The image and the chart set `/data/vmafx-controller.db`. |
| `XDG_STATE_HOME` | read directly | path | `~/.local/state` |  | Base of the default `VMAFX_DB_PATH` on Linux and the BSDs; a relative value is ignored. |
| `VMAFX_DB_DSN` | `db.dsn` | string | _(unset)_ | `controller.store.postgresql.mode`, `controller.store.postgresql.external.secretName`, `controller.store.postgresql.external.secretKey` | Secret. PostgreSQL connection string; required with `postgres` and by `vmafx-controller migrate` and `import-sqlite`. The standard `PG*` variables fill what it leaves out. |
| `VMAFX_STORE_LEASE_TTL` | `store.lease_ttl` | duration | `60s` | `controller.store.leaseTTL` | Lease of a pulled job (`postgres`); a negative value stops the controller. |
| `VMAFX_STORE_SESSION_TTL` | `store.session_ttl` | duration | `60s` | `controller.store.sessionTTL` | Lifetime of a node session without a heartbeat (`postgres`). |
| `VMAFX_STORE_SWEEP_INTERVAL` | `store.sweep_interval` | duration | `5s` | `controller.store.sweepInterval` | Period of the lease sweep (`postgres`). |
| `VMAFX_STORE_BACKOFF_BASE` | `store.backoff_base` | duration | `5s` | `controller.store.backoffBase` | Delay before a job whose lease expired is handed out again (`postgres`); doubles up to `VMAFX_STORE_BACKOFF_MAX`. |
| `VMAFX_STORE_BACKOFF_MAX` | `store.backoff_max` | duration | `5m` | `controller.store.backoffMax` | Cap of that delay (`postgres`). |
| `VMAFX_AUTH_DISABLED` | `auth.disabled` | bool | `false` | `auth.disabled` | Turn token verification off (development only): every call acts as tenant `dev` with the admin role. Refused with a tenant source. |
| `VMAFX_JWKS_ENDPOINT` | `jwks.endpoint` | URL | _(unset)_ | `auth.jwksEndpoint` | JWKS endpoint of the identity provider. Required unless auth is disabled or a tenant source is set (then refused) ([auth](auth.md)). |
| `VMAFX_AUTH_ISSUER` | `auth.issuer` | string | _(unset)_ | `auth.issuer` | Expected `iss` claim; required like `VMAFX_JWKS_ENDPOINT`. |
| `VMAFX_AUTH_AUDIENCE` | `auth.audience` | string | _(unset)_ | `auth.audience` | Expected `aud` claim; empty skips the check. Refused with a tenant source. |
| `VMAFX_AUTH_TENANT_CLAIM` | `auth.tenant_claim` | string | `tid` | `auth.tenantClaim` | Claim that carries the tenant ID. Refused with a tenant source. |
| `VMAFX_AUTH_ROLES_CLAIM` | `auth.roles_claim` | string | `vmafx_roles` | `auth.rolesClaim` | Claim that carries the list of roles. Refused with a tenant source. |
| `VMAFX_AUTH_TENANTS_SOURCE` | `auth.tenants.source` | string | _(unset)_ | set by the chart | `kubernetes` (the namespace's `VmafxTenant` resources) or `file`: verify each token against its tenant's provider ([tenant registry](auth.md#tenant-registry)). Excludes the provider variables above and `VMAFX_AUTH_DISABLED`. |
| `VMAFX_AUTH_TENANTS_FILE` | `auth.tenants.file` | path | _(unset)_ |  | YAML or JSON file of `VmafxTenant` documents (source `file`). |
| `VMAFX_AUTH_TENANTS_NAMESPACE` | `auth.tenants.namespace` | string | the pod's namespace | set by the chart | Namespace of the `VmafxTenant` resources (source `kubernetes`). |
| `VMAFX_AUTH_TENANTS_REFRESH` | `auth.tenants.refresh` | duration | `30s` |  | Re-read interval of the tenant source, `1s` to `1h`; the tenant set is refused after ten intervals without a successful read. |
| `VMAFX_SCORING_ROOTS` | `scoring.roots` | list | _(unset: every input refused)_ | `auth.scoringRoots` | Comma-separated scoring roots of every caller without a tenant registry; `{tenant}` becomes the caller's tenant ID ([scoring roots](auth.md#scoring-roots)). Refused with a tenant registry. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-controller -->

The table is generated from `api/vmafx-platform.toml`
([API generation](../development/api-generation.md#environment-of-the-go-binaries)).
How the authentication variables combine is on [auth.md](auth.md); the
controller does not start when they are inconsistent or a configured tenant is
invalid ([tenant registry](auth.md#tenant-registry)).

## VmafxScoring service (direct scoring)

Retained from Phase 4a for backward compatibility.  Clients that already talk to
`vmafx-server` continue to work against `vmafx-controller` without changes.

```protobuf
service VmafxScoring {
  rpc Score(ScoreRequest)   returns (ScoreResponse);
  rpc ScoreStream(stream ScoreStreamRequest)
      returns (stream ScoreStreamResponse);  // bidirectional, per-frame (ADR-0933)
  rpc Health(HealthRequest) returns (HealthResponse);
}
```

Proto: `proto/vmafx/v1/vmafx.proto`, generated from `api/vmafx-platform.toml`.
Generated stubs: `gen/go/` (package `vmafxv1`).

### Example: direct score

```bash
grpcurl -plaintext \
    -d '{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","model":"vmaf_v0.6.1"}' \
    localhost:9090 vmafx.v1.VmafxScoring/Score
```

## VmafxController service (job queue + node API)

Phase 4b.1 distributed orchestration surface. Proto:
`proto/vmafx/controller/v1/controller.proto`, generated from
`api/vmafx-platform.toml`. Generated stubs: `gen/go/controller/`.

### Client API

Used by the CLI, MCP server, and future Web UI to submit and track jobs. The
full service definition is in `api/vmafx-platform.toml`.

| RPC | Caller | Purpose |
| --- | --- | --- |
| `SubmitJob` | client | Enqueue a job for the caller's tenant; returns its ID |
| `GetJob` | client | Read the current state of one of the tenant's jobs |
| `CancelJob` | client | Cancel one of the tenant's pending or running jobs; a running job's node stops it (see [Cancelling a job](#cancel-a-job)) |
| `StreamJobs` | client | Server-streaming snapshot of the tenant's jobs (optional status filter); a snapshot in Phase 4b.1 |
| `RegisterNode` | node | Register a worker with its capability; the session belongs to the caller's tenant |
| `Heartbeat` | node | Keep the registration alive; the answer names the node's running jobs that were cancelled |
| `PullWork` | node | Receive the next matching job of the node's tenant |
| `ReportResult` | node | Report progress or the final result of a job assigned to the node |

Every call carries the tenant of its token, and every job it reads or writes
belongs to that tenant; the role each call needs and the tenant rules are in
[Auth gateway](auth.md#roles-and-rbac).

#### Submit a job

The reference and the distorted input must lie under the tenant's
[scoring roots](auth.md#scoring-roots), or the call fails with
`PERMISSION_DENIED`; the node checks them again, following symlinks, before
it reads them.

```bash
grpcurl -plaintext \
    -d '{"scoring":{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","backend":"cuda"}}' \
    localhost:9090 vmafx.controller.v1.VmafxController/SubmitJob
# → {"jobId": "550e8400-e29b-41d4-a716-446655440000"}
```

#### Poll job status

```bash
grpcurl -plaintext \
    -d '{"jobId":"550e8400-e29b-41d4-a716-446655440000"}' \
    localhost:9090 vmafx.controller.v1.VmafxController/GetJob
# → {"id":"...","status":"COMPLETED","scoring":{...},"assignedNode":"node-abc"}
```

#### Cancel a job

```bash
grpcurl -plaintext \
    -d '{"jobId":"550e8400-e29b-41d4-a716-446655440000"}' \
    localhost:9090 vmafx.controller.v1.VmafxController/CancelJob
# → {"ok":true,"message":"cancellation requested"}
```

The job becomes `CANCELLED` at once and stays so. A pending job is never
handed out. A running job keeps running on its node until the node's next
heartbeat, at most `VMAFX_CONTROLLER_HEARTBEAT_INTERVAL` (10 s) later: the
heartbeat lists the jobs the node runs, the controller answers with those
that were cancelled, and the node cancels them, which kills their `vmaf`
processes, and reports them as failed (`cancelled by the controller: ...`).
That report does not change the job's status
([ADR-1567](../adr/1567-job-cancel-reaches-node.md)).

### Node API

Used by `vmafx-node` worker processes to pull and report work (the node RPCs in
the table above).

Node lifecycle (`vmafx-node` implements it when `VMAFX_CONTROLLER_ADDR` is
set; see [node.md](node.md#pulling-jobs-from-the-controller)):

1. On startup, the node calls `RegisterNode` with its capability (GPU vendor,
   available backends, concurrency slots).  The controller returns a `node_id`
   and a `session_token`.
2. The node calls `Heartbeat` every ~10 s with the `node_id`, the
   `session_token` and the IDs of the jobs it runs (`running_job_ids`, at most
   64). The answer's `cancel_job_ids` names those of them that were cancelled;
   the node stops them.
   A node that misses heartbeats for 60 s is evicted, and its running jobs
   return to `PENDING` ahead of newer work, so another node picks them up. A
   node that comes back registers again; if it still finishes such a job, the
   first final result the controller receives is kept.
3. When the node has capacity, it calls `PullWork`.  The controller assigns the
   oldest `PENDING` job whose `backend` requirement matches the node's
   capabilities.
4. After the job completes (or fails), the node calls `ReportResult` with
   `final=true`.

### Job lifecycle

```figure
controller-job-lifecycle
```

### Backend capability matching

A job's `scoring.backend` field specifies which backend the job requires
(e.g. `"cuda"`, `"sycl"`, `"cpu"`).  If empty, any node can accept the job.
A node must list the required backend in its `capability.backends` to receive
the job.

## Job persistence

The controller keeps jobs and node sessions in one of two backends, chosen
with `VMAFX_STORE_BACKEND`:

| Backend | Where state lives | Replicas | Use |
| --- | --- | --- | --- |
| `sqlite` (default) | The embedded SQLite queue at `VMAFX_DB_PATH`; node sessions in memory | one | single host, development; transitional until the SQLite store of the standalone profile replaces it ([ADR-2350](../adr/2350-cloud-native-platform.md)) |
| `postgres` | PostgreSQL at `VMAFX_DB_DSN` (jobs, attempts, node sessions); River jobs in the same database | any number | clusters |

### PostgreSQL backend

```bash
export VMAFX_DB_DSN='postgres://vmafx:secret@db.example:5432/vmafx?sslmode=verify-full'
vmafx-controller migrate                       # schema of the store and of River
VMAFX_STORE_BACKEND=postgres vmafx-controller  # every replica the same way
```

- **Schema.** `vmafx-controller migrate` applies the store's migrations and
  River's, then exits; run it once per release, before the controllers of
  that release start. A controller whose database is below the schema it
  needs reports not ready on `/readyz` and logs both versions. A controller
  that starts before its database answers, or before River's tables exist,
  does not exit: it retries starting River every 2 s (for up to 30 minutes)
  and reports not ready until River runs, so its replicas can start in any
  order with the database and the migration.
- **Node work is leased.** `PullWork` gives a node a job with a lease
  (`VMAFX_STORE_LEASE_TTL`, default 60 s); every `Heartbeat` naming the job
  renews it. A lease that is not renewed expires: a sweep that runs every
  `VMAFX_STORE_SWEEP_INTERVAL` (default 5 s) on one replica returns the job to
  pending after a backoff (`VMAFX_STORE_BACKOFF_BASE` 5 s, doubling up to
  `VMAFX_STORE_BACKOFF_MAX` 5 min), and fails it after three lost leases.
- **One result per job.** A result is recorded only for the attempt the
  reporting session holds; a node that lost its lease and reports late is
  refused (`PERMISSION_DENIED`), and the job keeps the result of the attempt
  that replaced it. A retried report of an accepted result succeeds without a
  second write.
- **Sessions survive controller restarts.** The node ID returned by
  `RegisterNode` names a session stored in the database
  (`VMAFX_STORE_SESSION_TTL`, default 60 s), so a node keeps working through a
  controller restart or a replica failure without registering again. A node
  process that restarts registers again; the jobs it was running return to
  the queue when their leases expire.
- **Tenants.** Every query names the caller's tenant, and row-level security
  hides other tenants' rows from the database role the controller uses. That
  role must not be a superuser (a superuser bypasses row-level security).

### Moving from the SQLite queue

```bash
VMAFX_DB_DSN=... vmafx-controller import-sqlite --from /data/vmafx-controller.db
```

`import-sqlite` copies every job of the SQLite queue file (read-only) into
the PostgreSQL store: pending and running jobs arrive pending, finished jobs
keep their result, and a second run skips what the first copied. Jobs that
carry no tenant (a queue run with authentication disabled) are refused unless
`--empty-tenant <tenant>` names the tenant to give them.

### SQLite backend

On a restart of a controller with the SQLite backend:

- `PENDING` jobs are reloaded and re-queued in submission order.
- `RUNNING` jobs are reset to `PENDING` (their assigned nodes are gone).
- `COMPLETED`, `FAILED`, and `CANCELLED` jobs are retained for audit.

Schema: `cmd/vmafx-controller/queue/schema.sql`.

## Prometheus metrics

`/metrics` exposes the following in Prometheus exposition format. The scoring
counters and histogram are the metric set shared with `vmafx-server`
(`pkg/observability/observability.go`), so they carry the `vmafx_server_`
prefix; the queue and node metrics carry `vmafx_controller_`.

| Metric | Type | Description |
| --- | --- | --- |
| `vmafx_server_score_requests_total` | Counter | Direct Score requests (HTTP + gRPC VmafxScoring) |
| `vmafx_server_score_errors_total` | Counter | Direct Score requests that returned an error |
| `vmafx_server_score_duration_seconds` | Histogram | Direct scoring latency |
| `vmafx_server_health_requests_total` | Counter | Health / `/healthz` calls |
| `vmafx_server_ready_requests_total` | Counter | `/readyz` calls |
| `vmafx_controller_jobs_pending` | Gauge | Current number of PENDING jobs |
| `vmafx_controller_jobs_running` | Gauge | Current number of RUNNING jobs |
| `vmafx_controller_nodes_live` | Gauge | Current number of live registered nodes |
| `vmafx_controller_jobs_submitted_total` | Counter | Jobs submitted via SubmitJob RPC |
| `vmafx_controller_jobs_completed_total` | Counter | Jobs completed successfully |
| `vmafx_controller_jobs_failed_total` | Counter | Jobs that ended in failure |

There is no cancelled-jobs counter.

## Graceful shutdown

The controller listens for `SIGTERM` and `SIGINT`. On receipt it:

1. Stops accepting new connections.
2. Waits up to 30 seconds for in-flight requests to drain.
3. Exits with code 0.

Jobs remain in the SQLite database; the next controller instance reloads them.

## Further reading

- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — Phase 4b.1 decision record.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b
  umbrella architecture.
- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — Phase 4a origin
  (vmafx-server).
- [REST adapter](rest.md) — `/healthz`, `/readyz`, `/metrics`, `/v1/score`
  request and response shapes.
- [Auth gateway](auth.md) — JWT authentication and tenant isolation.
- [k8s deployment guide](../development/k8s-deployment.md) — Helm chart
  configuration.
