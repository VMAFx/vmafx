<!-- markdownlint-disable MD013 MD060 -->
# vmafx-server gRPC service

`vmafx-server` is a single Go binary that exposes VMAF scoring over both gRPC and HTTP/JSON.
This page covers the gRPC interface; see [http-transport.md](../mcp/http-transport.md) for the
HTTP endpoints (`/healthz`, `/readyz`, `/metrics`, `/v1/score`).

## Quick start

```bash
# Local dev (requires core/build-cpu to exist). Configuration is via the VMAFX_
# environment variables only — the pre-fx CLI flags were removed in ADR-1119.
VMAFX_VMAF_BINARY=core/build-cpu/tools/vmaf \
VMAFX_MODEL_DIR=model/ \
VMAFX_HTTP_ADDR=:8080 \
VMAFX_GRPC_LISTEN=:50051 \
go run ./cmd/vmafx-server

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
  rpc Score(ScoreRequest)   returns (ScoreResponse);                       // unary, file-path
  rpc ScoreStream(stream ScoreStreamRequest)                              // bidirectional, in-memory
      returns (stream ScoreStreamResponse);                               //   per-frame (ADR-0933)
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

## Streaming: `ScoreStream`

`ScoreStream` is a bidirectional RPC for per-frame scoring of in-memory raw
frames (no file round-trip). The client sends one `StreamConfig` (width, height,
pixel format, optional model), then a sequence of `FramePair` messages
(`frame_index` strictly increasing from 0; `raw_reference` / `raw_distorted` are
planar Y-U-V bytes), then half-closes. The server flushes the engine and streams
back one `FrameScore` per frame followed by a terminal `AggregateScore` (pooled
VMAF, per-feature pool, frame count, elapsed wall time).

Supported pixel formats: YUV 4:2:0 / 4:2:2 / 4:4:4 in 8-bit and 10-bit-LE. Each
`FramePair` payload must be exactly the configured frame size in bytes;
mismatches are rejected with `codes.InvalidArgument`. The same RPC is served by
`vmafx-node` ([ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md)).

See [grpc-streaming.md](../architecture/grpc-streaming.md) for the message-shape
table, the `pkg/score` client wrapper, and a worked client loop.

## Configuration

The server runs on the [golusoris](https://github.com/golusoris/golusoris) fx
framework (ADR-1119). Configuration is read by golusoris' koanf layer from
12-factor environment variables under the `VMAFX_` prefix; `_` in the env name
maps to the `.` config-key separator (so `VMAFX_HTTP_ADDR` sets `http.addr`).
The framework owns the listen sockets, so the HTTP and gRPC settings take **full
listen addresses** (`:8080`), not bare port numbers. There are no CLI flags.

| Env var | Config key | Default | Description |
|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `:8080` | HTTP listen address |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `:50051` | gRPC listen address |
| `VMAFX_LOG_LEVEL` | `log.level` | `INFO` | slog level (DEBUG/INFO/WARN/ERROR) |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | _(PATH lookup)_ | Path to the `vmaf` CLI binary |
| `VMAFX_MODEL_DIR` | `model.dir` | _(none)_ | Directory containing VMAF `.json` model files |
| `VMAFX_MAX_CONCURRENT_SCORES` | `max.concurrent.scores` | _(NumCPU)_ | Cap on simultaneous `Score` calls |

> **Config-key note.** The golusoris env transform strips the `VMAFX_` prefix,
> lowercases, and turns **every** `_` into the `.` delimiter — so
> `VMAFX_MODEL_DIR` lands under `model.dir` (not `vmaf.model_dir`) and
> `VMAFX_MAX_CONCURRENT_SCORES` under `max.concurrent.scores`. Set the
> environment variables shown in the first column; the second column is the
> resulting koanf key.
>
> **Breaking change (ADR-1119).** The pre-fx server used `VMAFX_PORT` /
> `VMAFX_GRPC_PORT` (bare port numbers) plus `--port` / `--grpc-port` CLI flags.
> These are gone. Use `VMAFX_HTTP_ADDR` / `VMAFX_GRPC_LISTEN` with full listen
> addresses (`:8080`). The historical gRPC default `:50051` is **preserved**:
> golusoris' `grpc.Module` defaults `grpc.listen` to `:9090`, but `vmafx-server`
> restores `:50051` when `VMAFX_GRPC_LISTEN` is unset so existing clients keep
> working without reconfiguration.

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
