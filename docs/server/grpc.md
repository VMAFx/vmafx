<!-- markdownlint-disable MD013 MD060 -->
# vmafx-server gRPC service

`vmafx-server` is a single Go binary that exposes VMAF scoring over both gRPC and HTTP/JSON.
This page covers the gRPC interface; see [http-transport.md](../mcp/http-transport.md) for the
HTTP endpoints (`/healthz`, `/readyz`, `/metrics`, `/v1/score`).

## Quick start

```bash
# Local dev (requires core/build-cpu to exist)
go run ./cmd/vmafx-server \
    --vmaf-binary core/build-cpu/tools/vmaf \
    --model-dir   model/ \
    --port        8080 \
    --grpc-port   50051

# Docker
docker build -f Dockerfile.go-server -t vmafx-server:dev .
docker run --rm \
    -e VMAFX_VMAF_BINARY=/usr/local/bin/vmaf \
    -e VMAFX_MODEL_DIR=/usr/local/share/vmafx/model \
    -p 8080:8080 -p 50051:50051 \
    vmafx-server:dev
```

## Proto definition

The canonical source of truth is `proto/vmafx.proto`. Generated stubs live under
`gen/go/` and are vendored in-tree.

```protobuf
service VmafxScoring {
  rpc Score(ScoreRequest)   returns (ScoreResponse);
  rpc Health(HealthRequest) returns (HealthResponse);
}

message ScoreRequest {
  string reference = 1;  // absolute path to reference YUV/Y4M
  string distorted = 2;  // absolute path to distorted YUV/Y4M
  string model     = 3;  // model name, e.g. "vmaf_v0.6.1" (default if empty)
}

message ScoreResponse {
  double              score    = 1;  // aggregate VMAF score
  map<string, double> features = 2;  // per-feature pooled-mean values
}
```

Regenerate stubs with:

```bash
buf generate proto   # requires buf ≥ v1.30 and the buf CLI on PATH
```

## Example: grpcurl

```bash
grpcurl -plaintext \
    -d '{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","model":"vmaf_v0.6.1"}' \
    localhost:50051 vmafx.v1.VmafxScoring/Score
```

Expected response (Netflix golden pair):

```json
{
  "score": 76.6683,
  "features": {
    "vmaf":       76.6683,
    "vif_scale0": 0.8912,
    "adm2":       0.9876,
    "motion2":    2.3456
  }
}
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

## Prometheus metrics

The `/metrics` endpoint exposes the following counters and histograms
in Prometheus exposition format, plus Go runtime and process metrics.

| Metric | Type | Description |
|---|---|---|
| `vmafx_server_score_requests_total` | Counter | Total Score requests (HTTP + gRPC) |
| `vmafx_server_score_errors_total` | Counter | Score requests that returned an error |
| `vmafx_server_score_duration_seconds` | Histogram | End-to-end scoring latency |
| `vmafx_server_health_requests_total` | Counter | Health / `/healthz` calls |
| `vmafx_server_ready_requests_total` | Counter | `/readyz` calls |

## Logging

All log lines are emitted as single-line JSON objects on stdout. Example:

```json
{"time":"2026-05-28T12:00:00.000Z","level":"INFO","msg":"grpc Score completed","score":"76.6683","duration_s":0.823}
```

## Graceful shutdown

The server listens for `SIGTERM` and `SIGINT`. On receipt it:

1. Stops accepting new connections.
2. Waits up to 30 seconds for in-flight requests to drain.
3. Exits with code 0.

## Relationship to the Python HTTP server (ADR-0701)

The Python `vmaf-mcp --transport http` server (PR #1583, ADR-0701) remains the default
transport for MCP/stdio IDE integrations and is **not removed by this PR**. The Go server
is an additive Phase-4 deliverable targeting k8s deployments where startup time and gRPC
are material. The Python layer will be retired in a separate Stage-3 cleanup PR after the
Go server confirms production parity.

## Further reading

- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — decision record for this service.
- [ADR-0701](../adr/0701-vmafx-cloud-native-redesign.md) — Python HTTP transport foundation.
- [HTTP transport docs](../mcp/http-transport.md) — `/healthz`, `/readyz`, `/metrics`, `/v1/score`.
- [k8s deployment guide](../development/k8s-deployment.md) — Helm chart configuration.
