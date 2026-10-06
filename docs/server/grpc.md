<!-- markdownlint-disable MD013 MD060 -->
# vmafx-server gRPC service

`vmafx-server` is a single Go binary that exposes VMAF scoring over both gRPC
(default `:9090`) and HTTP/JSON (default `:8080`). This page covers the gRPC
interface; see [rest.md](rest.md) for the HTTP endpoints (`/v1/score`,
`/healthz`, `/readyz`, `/metrics`, ...).

## Quick start

1. Run the server locally. It needs a `vmaf` binary, for example from
   `core/build-cpu`. Configuration is by `VMAFX_` environment variables only;
   the pre-fx CLI flags were removed in ADR-1119.

    ```bash
    VMAFX_VMAF_BINARY=core/build-cpu/tools/vmaf \
    VMAFX_MODEL_DIR=model/ \
    VMAFX_HTTP_ADDR=:8080 \
    VMAFX_GRPC_LISTEN=:9090 \
    go run ./cmd/vmafx-server
    ```

2. Or run the published release image (multi-architecture amd64/arm64). Use
   a release tag such as `v1.0.0-rc.2` or `latest`.
   Images published after `v1.0.0-rc.2` have zstd layers and need Docker
   Engine 23.0 or later, Docker Desktop 4.19 or later, Podman or containerd 1.5
   or later ([what can pull them](../usage/docker.md#what-can-pull-the-images)).

    ```bash
    docker run --rm \
        -p 8080:8080 -p 9090:9090 \
        ghcr.io/vmafx/vmafx-server:v1.0.0-rc.2
    ```

3. Verify the release version without starting listeners.

    ```bash
    docker run --rm ghcr.io/vmafx/vmafx-server:v1.0.0-rc.2 --version
    ```

4. To test a local build, build the image and inject the version explicitly
   when you test release behaviour.

    ```bash
    docker build -f Dockerfile.go-server \
        --build-arg VMAFX_VERSION=dev \
        -t vmafx-server:dev .
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
  string model     = 3;  // model name, e.g. "vmaf_v0.6.1"; empty defaults to vmaf_v1.0.16_3d0h
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
    localhost:9090 vmafx.v1.VmafxScoring/Score
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
frames, with no file round-trip. The message flow:

1. The client sends one `StreamConfig` (width, height, pixel format, optional
   model).
2. The client sends a sequence of `FramePair` messages: `frame_index` strictly
   increasing from 0, `raw_reference` / `raw_distorted` as planar Y-U-V bytes.
3. The client half-closes.
4. The server flushes the engine and streams back one `FrameScore` per frame,
   then a terminal `AggregateScore` (pooled VMAF, per-feature pool, frame
   count, elapsed wall time).

Supported pixel formats: YUV 4:2:0 / 4:2:2 / 4:4:4 in 8-bit and 10-bit-LE. Each
`FramePair` payload must be exactly the configured frame size in bytes;
mismatches are rejected with `codes.InvalidArgument`. The same RPC is served by
`vmafx-node` ([ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md)).

See [grpc-streaming.md](../architecture/grpc-streaming.md) for the message-shape
table, the `pkg/score` client wrapper, and a worked client loop.

## Configuration

The server runs on the [golusoris](https://github.com/golusoris/golusoris) fx
framework (ADR-1119). Runtime configuration is environment-only: golusoris'
koanf layer reads 12-factor variables under the `VMAFX_` prefix, and `_` in the
env name maps to the `.` config-key separator (so `VMAFX_HTTP_ADDR` sets
`http.addr`).

- Source precedence, the underscore rule and the default HTTP and gRPC limits
  are on [Server configuration](configuration.md).
- The framework owns the listen sockets, so the HTTP and gRPC settings take
  **full listen addresses** (`:8080`), not bare port numbers.
- The sole process switch is `--version`. It prints the build-time version and
  exits without constructing the fx application or binding listeners.

| Env var | Config key | Default | Description |
| --- | --- | --- | --- |
| `VMAFX_HTTP_ADDR` | `http.addr` | `:8080` | HTTP listen address |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `:9090` | gRPC listen address |
| `VMAFX_LOG_LEVEL` | `log.level` | `INFO` | slog level (DEBUG/INFO/WARN/ERROR) |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | _(PATH lookup)_ | Path to the `vmaf` CLI binary |
| `VMAFX_MODEL_DIR` | `model.dir` | _(none)_ | Directory containing VMAF `.json` model files |
| `VMAFX_MAX_CONCURRENT_SCORES` | `max.concurrent.scores` | _(NumCPU)_ | Cap on simultaneous `Score` calls |

!!! note "Config-key note"
    The golusoris env transform strips the `VMAFX_` prefix, lowercases, and
    turns **every** `_` into the `.` delimiter. `VMAFX_MODEL_DIR` therefore
    lands under `model.dir` (not `vmaf.model_dir`) and
    `VMAFX_MAX_CONCURRENT_SCORES` under `max.concurrent.scores`. Set the
    environment variables shown in the first column; the second column is the
    resulting koanf key.

!!! warning "Breaking change (ADR-1119)"
    The pre-fx server used `VMAFX_PORT` / `VMAFX_GRPC_PORT` (bare port
    numbers) plus `--port` / `--grpc-port` CLI flags. These are gone. Use
    `VMAFX_HTTP_ADDR` / `VMAFX_GRPC_LISTEN` with full listen addresses
    (`:8080`). The gRPC default moved from the historical `:50051` to
    golusoris' native `:9090`; set `VMAFX_GRPC_LISTEN=:50051` explicitly while
    migrating existing clients.

## Prometheus metrics

The `/metrics` endpoint exposes the following counters and histograms
in Prometheus exposition format, plus Go runtime and process metrics.

| Metric | Type | Description |
| --- | --- | --- |
| `vmafx_server_score_requests_total` | Counter | Total Score requests (HTTP + gRPC) |
| `vmafx_server_score_errors_total` | Counter | Score requests that returned an error |
| `vmafx_server_score_duration_seconds` | Histogram | End-to-end scoring latency |
| `vmafx_server_health_requests_total` | Counter | Health / `/healthz` calls |
| `vmafx_server_ready_requests_total` | Counter | `/readyz` calls |

## Logging

The log format follows `VMAFX_LOG_FORMAT` (`auto`, `tint` or `json`; default
`auto`). With the `json` handler each line is a single-line JSON object, for
example:

```json
{"time":"2026-05-28T12:00:00.000Z","level":"INFO","msg":"http Score completed","request_id":"4bf92f3577b34da6","route":"POST /v1/score","model":"vmaf_v0.6.1","score":"76.6683","duration_s":0.823}
```

Request-scoped lines use one set of field names (`pkg/observability/logfields.go`):

| Field | Meaning |
| --- | --- |
| `request_id` | The OpenTelemetry trace id when the request is traced, otherwise a random 16-hex-digit id. Every line of one request carries the same value. |
| `rpc` | gRPC method without the package: `Score`, `ScoreStream`, `Health`. |
| `route` | HTTP route as `<METHOD> <path>`, for example `POST /v1/score`. |
| `model` | VMAF model the request named. |
| `backend` | Compute backend: `cpu`, `cuda`, `sycl`, `hip`, `metal`. |
| `duration_s` | Elapsed seconds, as a float. |
| `error` | The error value. |

The legacy `POST /v1/score` path logs this set today; the gRPC and OpenAPI
REST paths adopt it as their log calls move onto the same helpers.

## Graceful shutdown

The server listens for `SIGTERM` and `SIGINT`. On receipt it:

1. Stops accepting new connections.
2. Waits up to 30 seconds for in-flight requests to drain.
3. Exits with code 0.

## Relationship to the Python HTTP server (ADR-0701)

The Python `vmaf-mcp --transport http` server (PR #1583, ADR-0701) remains the
default transport for MCP/stdio IDE integrations and is not removed by the Go
server. The Go server is an additive Phase-4 deliverable targeting Kubernetes
deployments where startup time and gRPC are material. See
[MCP HTTP transport](../mcp/http-transport.md) for the Python server.

## Further reading

- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — decision record for this
  service.
- [ADR-0701](../adr/0701-vmafx-cloud-native-redesign.md) — Python HTTP transport
  foundation.
- [REST adapter](rest.md) — `/healthz`, `/readyz`, `/metrics`, `/v1/score`.
- [Python MCP HTTP transport](../mcp/http-transport.md) — the separate Python
  REST mode.
- [k8s deployment guide](../development/k8s-deployment.md) — Helm chart
  configuration.
