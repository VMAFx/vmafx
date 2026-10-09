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

The services are defined in `api/vmafx-platform.toml`, from which the generator
writes `proto/vmafx/v1/vmafx.proto` (the scoring service) and
`proto/vmafx/controller/v1/controller.proto` (the controller). The scoring
options (`ScoreOptions`) and the provenance record (`Provenance`) come from
`proto/vmafx/v1/vmafx_api.proto`, which is generated from `core/api/vmafx.toml`;
the [scoring API contract](api-contract.md) lists every option and says how the
API may change. Go stubs live under `gen/go/` and are vendored in-tree; see
[API generation](../development/api-generation.md#platform-definition) for how
they are regenerated.

```protobuf
service VmafxScoring {
  rpc Score(ScoreRequest)   returns (ScoreResponse);                       // unary, file-path
  rpc ScoreStream(stream ScoreStreamRequest)                              // bidirectional, in-memory
      returns (stream ScoreStreamResponse);                               //   per-frame (ADR-0933)
  rpc Health(HealthRequest) returns (HealthResponse);
}

message ScoreRequest {
  string reference = 1;          // absolute path to reference YUV/Y4M
  string distorted = 2;          // absolute path to distorted YUV/Y4M
  string model     = 3;          // model name, e.g. "vmaf_v0.6.1"; empty defaults to vmaf_v1.0.16_3d0h
  ScoreOptions options = 4;      // geometry, backend, threads, precision, ... (generated)
}

message ScoreResponse {
  double              score      = 1;  // aggregate VMAF score
  map<string, double> features   = 2;  // per-feature pooled-mean values
  ScoreProvenance     provenance = 3;  // how the score was made; in every response
}
```

Scores are lossless (`options.precision` `max`) unless the request names
another precision. A raw `.yuv` pair needs `width`, `height`, `pixel_format`
and `bitdepth` in `options`; an option value the contract refuses is
`INVALID_ARGUMENT`.

Regenerate stubs after changing either proto file (the generated messages come
from `python3 scripts/codegen/vmafx-api.py --write` first):

```bash
buf generate proto   # requires buf ≥ v1.30 and the buf CLI on PATH
python3 scripts/proto/postprocess_gen_go.py
```

## Example: grpcurl

```bash
grpcurl -plaintext \
    -d '{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv","model":"vmaf_v0.6.1",
         "options":{"width":576,"height":324,"pixelFormat":"420","bitdepth":8}}' \
    localhost:9090 vmafx.v1.VmafxScoring/Score
```

Expected response (Netflix golden pair; `grpcurl` prints proto JSON names in
lowerCamelCase):

```json
{
  "score": 76.66783149135578,
  "features": {
    "vmaf":            76.66783149135578,
    "integer_adm2":    0.9345057762923995,
    "integer_motion2": 3.894360826611791
  },
  "provenance": {
    "library": {"abiMinor": 1, "abiPatch": 4, "activeBackend": "cpu", "nExtractors": 3,
                "version": "v1.0.0-rc.2-529-gb8b3af281"},
    "model": "vmaf_v0.6.1",
    "modelSha256": "5950d61fa1f861bd45d8149d80539ed9f3376cfc2495b8f0fa8e9f57cb131ee3",
    "backendUsed": "cpu",
    "featureBackends": [{"extractor": "adm", "backend": "cpu"}],
    "precision": "max"
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

<!-- BEGIN GENERATED: vmafx-api environment vmafx-server (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:8080` |  | HTTP listen address of the REST API, `/swagger`, `/metrics` and the probes, a full address. |
| `VMAFX_HTTP_TIMEOUTS_READ` | `http.timeouts.read` | duration | `30s` |  | Deadline for reading a whole request; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_HEADER` | `http.timeouts.header` | duration | `5s` |  | Deadline for reading the request headers (slow-client guard); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` | duration | `15m` |  | Deadline for writing a response; raised so a long `POST /v1/score` completes ([limits](configuration.md#limits-and-timeouts)); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_IDLE` | `http.timeouts.idle` | duration | `120s` |  | Keep-alive idle timeout; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_SHUTDOWN` | `http.timeouts.shutdown` | duration | `30s` |  | Drain time of the HTTP server at shutdown; `0` keeps the framework default. |
| `VMAFX_HTTP_LIMITS_HEADER` | `http.limits.header` | bytes | `1048576` |  | Largest request header block; `0` keeps the default. |
| `VMAFX_HTTP_LIMITS_BODY` | `http.limits.body` | bytes | `10485760` |  | Largest request body; `0` keeps the default. `VMAFX_HTTP_LIMITS_UNLIMITED` removes the cap. |
| `VMAFX_HTTP_LIMITS_UNLIMITED` | `http.limits.unlimited` | bool | `false` |  | Serve request bodies of any size; `VMAFX_HTTP_LIMITS_BODY` is then ignored. |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:9090` |  | gRPC listen address of `VmafxScoring`, a full address. |
| `VMAFX_GRPC_TLS` | `grpc.tls` | bool | `false` |  | Serve gRPC over TLS; needs `VMAFX_GRPC_CERT_FILE` and `VMAFX_GRPC_KEY_FILE`. |
| `VMAFX_GRPC_CERT_FILE` | `grpc.cert_file` | path | _(unset)_ |  | PEM certificate of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_KEY_FILE` | `grpc.key_file` | path | _(unset)_ |  | PEM private key of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_MAX_RECV_SIZE` | `grpc.max_recv_size` | bytes | `67108864` |  | Largest gRPC message received; `0` keeps the gRPC default. |
| `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.max_send_size` | bytes | `4194304` |  | Largest gRPC message sent; `0` keeps the gRPC default. |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | `vmaf` on `PATH` |  | Path of the `vmaf` CLI behind the scorer; a missing binary stops the program at startup. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(unset)_ |  | Directory of the VMAF `.json` models the scorer loads. |
| `VMAFX_MAX_CONCURRENT_SCORES` | `max.concurrent.scores` | integer | number of CPUs |  | Cap on simultaneous `Score` calls over HTTP and gRPC; excess calls get HTTP 429 or `ResourceExhausted`. A value below 1 or not a number keeps the default. |
| `VMAFX_SWAGGER_TRY_IT_OUT` | read directly | `1` | off |  | `1` enables the live "try it out" execution of the Swagger UI. |
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

<!-- END GENERATED: vmafx-api environment vmafx-server -->

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
