# vmafx-server REST API

`vmafx-server` exposes VMAF scoring over a REST/JSON transport in addition to
its gRPC interface.  Both transports share the same underlying scoring engine
(`pkg/libvmaf`) so results are identical regardless of which transport is used.

The formal REST contract is maintained as an OpenAPI 3.0.3 specification at
[`api/openapi/vmafx-server-v1.yaml`](../../api/openapi/vmafx-server-v1.yaml).
Generated Go server stubs live at
[`gen/go/oapi/vmafx_server_v1.gen.go`](../../gen/go/oapi/vmafx_server_v1.gen.go)
and are produced by
[oapi-codegen](https://github.com/oapi-codegen/oapi-codegen).

See [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) for the overall server
design and [ADR-0797](../adr/0797-openapi-rest-schema.md) for the OpenAPI
contract decisions.

## Quick start

1. Start the server. Configuration is by `VMAFX_*` environment variables only;
   the pre-fx CLI flags were removed in ADR-1119 (see
   [grpc.md](grpc.md#configuration)).

    ```bash
    VMAFX_VMAF_BINARY=core/build-cpu/tools/vmaf \
    VMAFX_MODEL_DIR=model/ \
    VMAFX_HTTP_ADDR=:8080 \
    go run ./cmd/vmafx-server
    ```

2. Open the Swagger UI.

    ```bash
    xdg-open http://localhost:8080/swagger
    ```

3. Score a pair.

    ```bash
    curl -s -X POST http://localhost:8080/v1/score \
         -H 'Content-Type: application/json' \
         -d '{"reference":"/data/ref.yuv","distorted":"/data/dis.yuv"}' \
         | jq .
    ```

## Endpoints

All endpoints return `application/json` unless noted.

| Method | Path                 | Description                                           |
| ------ | -------------------- | ----------------------------------------------------- |
| `POST` | `/v1/score`          | Score a reference/distorted video pair                |
| `GET`  | `/v1/health`         | Liveness probe — always 200 while the process is live |
| `GET`  | `/v1/ready`          | Readiness probe — 200 once the scorer is initialised  |
| `GET`  | `/swagger`           | Interactive Swagger UI                                |
| `GET`  | `/swagger/spec.json` | Embedded OpenAPI spec (JSON)                          |
| `GET`  | `/metrics`           | Prometheus exposition format                          |

### Legacy aliases (Kubernetes conventions)

The following aliases are retained for backwards compatibility with
deployments that target the bare probe paths:

| Alias      | Canonical endpoint |
| ---------- | ------------------ |
| `/healthz` | `/v1/health`       |
| `/readyz`  | `/v1/ready`        |

`/readyz` is stricter than `/v1/ready`: besides the scorer it also fails (503)
when the `vmaf` binary has been removed or is no longer executable, or when
`VMAFX_MODEL_DIR` is not a directory. See
[Server configuration](configuration.md#readiness).

## POST /v1/score

**Request body** (`application/json`):

```json
{
  "reference": "/data/src01_hrc00_576x324.yuv",
  "distorted": "/data/src01_hrc01_576x324.yuv",
  "model": "vmaf_v0.6.1",
  "options": {"width": 576, "height": 324, "pixel_format": "420", "bitdepth": 8}
}
```

| Field | Type | Required | Description |
| --- | --- | --- | --- |
| `reference` | string | Yes | Path to the pristine reference video |
| `distorted` | string | Yes | Path to the compressed distorted video |
| `model` | string | No | Model name (default: `vmaf_v1.0.16_3d0h`, the library default) |
| `options` | object | No | Scoring options (`ScoreOptions`): raw `.yuv` geometry, backend, threads, precision, features and more; see the [scoring API contract](api-contract.md#option-reference) |

The body is the gRPC `ScoreRequest` in JSON with the proto field names; a
field the contract does not know is refused.

**Response** `200 OK` (the gRPC `ScoreResponse` with proto field names;
scores are lossless unless `options.precision` names another precision):

```json
{
  "score": 76.66783149135578,
  "features": {"vmaf": 76.66783149135578, "integer_adm2": 0.9345057762923995},
  "provenance": {
    "library": {"abi_minor": 1, "abi_patch": 4, "active_backend": "cpu", "n_extractors": 3,
                "version": "v1.0.0-rc.2-529-gb8b3af281"},
    "model": "vmaf_v0.6.1",
    "model_sha256": "5950d61fa1f861bd45d8149d80539ed9f3376cfc2495b8f0fa8e9f57cb131ee3",
    "backend_used": "cpu",
    "feature_backends": [{"extractor": "adm", "backend": "cpu"}],
    "precision": "max"
  }
}
```

`provenance` is in every response; the [scoring API contract](api-contract.md#responses-and-provenance)
describes each field.

**Error responses**:

| Code | Condition |
| --- | --- |
| `400` | Missing `reference` or `distorted`, invalid JSON body, an unknown field, or an option value the contract refuses |
| `413` | Request body larger than 1 MiB |
| `429` | Concurrency cap reached (`VMAFX_MAX_CONCURRENT_SCORES`) |
| `500` | Internal scoring error (check server logs) |

## Swagger UI

Navigate to `http://<host>:8080/swagger` in a browser.  The UI renders the
embedded OpenAPI spec from `/swagger/spec.json`.

The **Try it out** button is disabled by default to prevent accidental
execution against production servers.  To enable it:

```bash
VMAFX_SWAGGER_TRY_IT_OUT=1 go run ./cmd/vmafx-server
```

The Swagger UI is served via CDN (`unpkg.com/swagger-ui-dist`, the version
pinned by `swaggerUIVersion` in `cmd/vmafx-server/swagger_ui.go`).  In
air-gapped environments, proxy the CDN URL through a local mirror and rebuild
the binary with the updated `swaggerUIBaseURL` constant in
`cmd/vmafx-server/swagger_ui.go`.

## Regenerating the Go stubs

When `api/openapi/vmafx-server-v1.yaml` is modified, regenerate the stubs:

```bash
oapi-codegen \
    --config api/openapi/oapi-codegen.yaml \
    api/openapi/vmafx-server-v1.yaml
```

Requires `oapi-codegen` v2:

```bash
go install github.com/oapi-codegen/oapi-codegen/v2/cmd/oapi-codegen@latest
```

## REST adapter architecture

The REST adapter (`cmd/vmafx-server/rest_adapter.go`) is maintainer material; it
implements the
`oapi.ServerInterface` generated by oapi-codegen. Each scoring call reads the
body as the proto `ScoreRequest` (proto field names, which are the OpenAPI
property names) and delegates to the shared `grpcServer` instance; the served
`POST /v1/score` handler and gRPC `Score` reach the same scoring path
(`runScore` in `cmd/vmafx-server/score_options.go`). This ensures:

- Business logic (scoring, Prometheus metrics, structured logging) lives in
  exactly one place (`grpcServer`).
- Adding a new REST endpoint only requires updating the OpenAPI spec,
  regenerating stubs, and adding a thin translation method in `restAdapter`.

```text
HTTP request
    → net/http ServeMux
    → oapi.ServerInterfaceWrapper (generated middleware)
    → restAdapter.ScoreVideoPair / GetHealth / GetReady
    → grpcServer.Score / Health        ← single source of truth
    → pkg/libvmaf.Scorer
```
