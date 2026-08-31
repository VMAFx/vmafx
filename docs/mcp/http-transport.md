<!-- markdownlint-disable MD036 MD060 -->
# vmafx-mcp HTTP transport

> **Added in**: VMAFX Phase 3A (ADR-0701)
> **Default transport**: stdio (unchanged for IDE/MCP-client compatibility)

The `vmaf-mcp` / `vmafx-mcp` server can run in HTTP server mode by passing
`--transport http`. This mode exposes a REST API suitable for Kubernetes
liveness/readiness probes, Prometheus scraping, and direct curl-based scoring.

---

## Quick start

```bash
# Install with HTTP extras
pip install 'vmaf-mcp[http]'

# Start the server on port 8080 with bearer authentication
VMAFX_MCP_HTTP_TOKEN='replace-with-a-secret' \
  vmaf-mcp --transport http --port 8080

# Or via environment variable
VMAFX_PORT=8080 VMAFX_MCP_HTTP_TOKEN='replace-with-a-secret' \
  vmaf-mcp --transport http
```

---

## Endpoint reference

### `GET /healthz` — Liveness probe

Returns `200 OK` while the process is alive after the request passes the shared
authentication middleware. Suitable for an authenticated Kubernetes
`livenessProbe`.

**Response**

```json
{ "status": "healthy" }
```

---

### `GET /readyz` — Readiness probe

Returns `200 OK` once the configured vmaf binary is reachable on the filesystem.
Returns `503 Service Unavailable` if the binary is absent. Suitable for Kubernetes
`readinessProbe`.

The check is a lightweight `stat` call — no subprocess is spawned.

**Response (ready)**

```json
{ "status": "ready", "vmaf_binary": "/usr/local/bin/vmaf" }
```

**Response (not ready)**

```json
{ "status": "not_ready", "reason": "vmaf binary not found at /usr/local/bin/vmaf" }
```

---

### `GET /metrics` — Prometheus metrics

Returns metrics in [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/).
Suitable for `prometheusRule` scraping.

**Exposed metrics**

| Metric | Type | Description |
|---|---|---|
| `vmaf_scoring_requests_total{endpoint, status}` | Counter | Total scoring requests, labelled by endpoint and HTTP status |
| `vmaf_scoring_errors_total` | Counter | Total scoring requests that resulted in a 500-level error |
| `vmaf_scoring_duration_seconds` | Histogram | Scoring request latencies (buckets: 0.1s … 300s) |

---

### `POST /v1/score` — Score a YUV pair

Submits a VMAF scoring request for a raw YUV pair. This is a thin REST wrapper
over the `vmaf_score` MCP tool.

**Request body (JSON)**

| Field | Type | Required | Description |
|---|---|---|---|
| `reference` | string | yes | Absolute path to the reference YUV file |
| `distorted` | string | yes | Absolute path to the distorted YUV file |
| `width` | integer | yes | Frame width in pixels |
| `height` | integer | yes | Frame height in pixels |
| `pixfmt` | string | yes | Pixel format: `"420"`, `"422"`, or `"444"` |
| `bitdepth` | integer | yes | Bit depth: `8` \| `10` \| `12` \| `16` |
| `model` | string | no | Model specifier (default: `"version=vmaf_v0.6.1"`) |
| `backend` | string | no | Backend: `"cpu"`, `"cuda"`, `"sycl"`, or `"auto"` (default: `"auto"`) |
| `precision` | string | no | Output precision: `"legacy"` (`%.6f`, the C-CLI default per ADR-0119) or `"max"` (lossless `%.17g`). Default: `"legacy"` |

**Example request**

```bash
curl -X POST http://localhost:8080/v1/score \
  -H 'Authorization: Bearer replace-with-a-secret' \
  -H 'Content-Type: application/json' \
  -d '{
    "reference": "/data/ref.yuv",
    "distorted": "/data/dis.yuv",
    "width": 1920,
    "height": 1080,
    "pixfmt": "420",
    "bitdepth": 8
  }'
```

**Response (200 OK)**

The vmaf JSON payload plus a `request_id` field:

```json
{
  "vmaf": 85.432,
  "frames": [ ... ],
  "pooled_metrics": { ... },
  "request_id": "a3f9c21b"
}
```

**Error responses**

| Status | Condition |
|---|---|
| `400` | Missing required fields, invalid JSON body (including non-object JSON values such as `null`, arrays, or integers), or path outside allowlisted roots |
| `401` | Missing or invalid `Authorization: Bearer` token (when auth is enabled) |
| `413` | Request body exceeds 4 MiB (enforced by both `Content-Length` pre-flight and `client_max_size` for chunked bodies) |
| `500` | Scoring subprocess failed |

---

## Environment variable reference

CLI flags take precedence over environment variables; environment variables take
precedence over compiled-in defaults.

| Variable | Default | Description |
|---|---|---|
| `VMAFX_PORT` | `8080` | HTTP listen port (overridden by `--port`) |
| `VMAFX_LOG_LEVEL` | `INFO` | Python log level: `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| `VMAFX_VMAF_BINARY` | *(auto-detected)* | Explicit path to the `vmaf` binary; falls through to `VMAF_BIN` |
| `VMAFX_MODEL_DIR` | *(none)* | Additional model search root; appended to `VMAF_MCP_ALLOW` |

### Security (ADR-0967)

These variables harden the HTTP transport and are honoured **identically** by
both the Python (`vmaf-mcp`) and the Go (`vmafx-mcp`) servers, so a single
deployment config secures either implementation:

| Variable | Default | Description |
|---|---|---|
| `VMAFX_MCP_HTTP_TOKEN` | *(none)* | Bearer token. When set (and `NO_AUTH` is unset), every request must carry `Authorization: Bearer <token>`, matched in constant time. |
| `VMAFX_MCP_HTTP_NO_AUTH` | *(unset)* | Set to `1` to disable authentication entirely (explicit operator opt-out). |
| `VMAFX_MCP_HTTP_BIND` | `127.0.0.1` | Bind host. Loopback-only by default; set to `0.0.0.0` to listen on all interfaces. |

When neither `VMAFX_MCP_HTTP_TOKEN` nor `VMAFX_MCP_HTTP_NO_AUTH=1` is set, the
server **rejects every request with 401** — a missing token means auth was not
configured, and refusing is safer than silently accepting. The request body is
capped at 4 MiB (`413` on overflow). For the Go server, `VMAFX_MCP_HTTP_BIND`
only substitutes the host when the configured `VMAFX_MCP_HTTP_ADDR` (e.g.
`:3000`) carries no explicit host; an address that already pins a host wins.

---

## Structured JSON logging

HTTP mode replaces the root logger's handlers with a single-line JSON formatter.
Each log line is a JSON object with the following fields:

| Field | Example | Description |
|---|---|---|
| `timestamp` | `"2026-05-28T12:34:56.789Z"` | ISO-8601 with millisecond precision |
| `level` | `"INFO"` | Python log level |
| `message` | `"POST /v1/score done in 420ms"` | Human-readable message |
| `request_id` | `"a3f9c21b"` | 8-character hex request identifier (or `"-"` for server-level events) |
| `logger` | `"vmafx.http"` | Logger name |
| `module` | `"http_transport"` | Source module |
| `lineno` | `303` | Source line number |

---

## SIGTERM behaviour and graceful shutdown

On receiving `SIGTERM` or `SIGINT`, the server:

1. Logs `"SIGTERM received — initiating graceful shutdown"`.
2. Stops the `asyncio` event loop.
3. Calls `AppRunner.cleanup()` to drain in-flight requests and close the TCP
   listener.

The cleanup runs within the event loop's `finally` block; there is no separate
hard timeout enforced at the transport layer beyond the `asyncio` task
cancellation semantics. Kubernetes pods should set `terminationGracePeriodSeconds`
to at least 30 seconds to allow long-running scoring requests to complete.

---

## Optional dependencies

HTTP mode requires the `[http]` extra, which is not installed by default:

```bash
pip install 'vmaf-mcp[http]'
# or equivalently:
pip install 'vmaf-mcp[http]' aiohttp>=3.9 prometheus-client>=0.20
```

If `aiohttp` or `prometheus-client` is absent and `--transport http` is
requested, the server raises an `ImportError` with an installation hint.
The published `vX.Y.Z-server` image installs `[eval,http]`, so its default
HTTP entrypoint includes both dependencies. Its image configuration explicitly
sets `VMAFX_MCP_HTTP_BIND=0.0.0.0`; standalone Python installs retain the safer
`127.0.0.1` default.

---

## Kubernetes deployment

For a full Kubernetes deployment, see:

- [deploy/helm/vmafx/](../../deploy/helm/vmafx/) — Helm chart (ADR-0699)
- [dev/Containerfile](../../dev/Containerfile) — production Dockerfile (ADR-0698)

The Helm chart sets `VMAFX_PORT`, configures liveness and readiness probes
against `/healthz` and `/readyz`, and wires a `ServiceMonitor` for Prometheus
scraping of `/metrics`.

---

## See also

- [ADR-0701](../adr/0701-vmafx-cloud-native-redesign.md) — design decisions for
  this transport.
- [MCP tools reference](tools.md) — full list of MCP JSON-RPC tools available over
  the default stdio transport.
- [MCP backends](backends.md) — backend selection for scoring.
