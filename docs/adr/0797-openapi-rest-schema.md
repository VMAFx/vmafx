# ADR-0797: vmafx-server OpenAPI REST contract

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: server, api, rest, openapi, swagger, go, vmafx-server

## Context

`vmafx-server` (ADR-0703) exposes VMAF scoring over both gRPC and a
hand-written HTTP transport (`/healthz`, `/readyz`, `/metrics`, `/v1/score`).
The HTTP surface has no formal contract: there is no machine-readable schema,
no request-validation middleware, and no interactive documentation for
operators.  This creates friction when integrating the server into CI
pipelines, Kubernetes admission controllers, or external clients that need to
generate typed bindings.

PR #101 closes that gap by adding an OpenAPI 3.0.3 spec, oapi-codegen-generated
Go stubs, a REST adapter that delegates to the existing gRPC handlers, and a
CDN-hosted Swagger UI.

## Decision

We will ship a formal OpenAPI 3.0.3 specification at
`api/openapi/vmafx-server-v1.yaml` covering every v1 REST endpoint.
Go server stubs will be generated via oapi-codegen v2 into
`gen/go/oapi/vmafx_server_v1.gen.go`.  A thin `restAdapter` in
`cmd/vmafx-server/rest_adapter.go` implements the generated
`oapi.ServerInterface` by delegating to the shared `grpcServer` instance, so
business logic (scoring, metrics, logging) is never duplicated.  The existing
legacy endpoints (`/healthz`, `/readyz`, `/v1/score`) are preserved for
backwards compatibility.  The Swagger UI is mounted at `/swagger` using the
CDN-hosted `swagger-ui-dist` bundle pinned to v5.18.2; the embedded spec JSON
is served at `/swagger/spec.json` without any filesystem dependency.
Try-it-out is disabled by default and can be enabled via
`VMAFX_SWAGGER_TRY_IT_OUT=1`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Hand-write REST handlers (status quo) | No new dependencies | No machine-readable contract; hard to keep in sync with gRPC proto | Rejected: the lack of a schema is the problem being solved |
| grpc-gateway (grpc → REST transcoding) | Single source of truth (proto); REST endpoints auto-generated | Large runtime dependency; REST URL style constrained by proto field names; introduces another gateway layer | Rejected: oapi-codegen produces lighter stubs and lets the spec evolve independently of the proto |
| OpenAPI 3.1.0 | Latest spec version; native JSON Schema refs | oapi-codegen v2.7 officially unsupported (warning emitted); kin-openapi loader has partial 3.1 support | Rejected: downgrade to 3.0.3 eliminates warnings and ensures full tooling compatibility with no functional loss for our schema |
| Serve Swagger UI from vendored static assets | No CDN dependency | ~5 MB of JS/CSS added to the tree | Rejected: CDN-hosted with a pinned version achieves the same reproducibility without repository bloat; operators without internet can set `VMAFX_SWAGGER_TRY_IT_OUT` and proxy the CDN via a corporate mirror |

## Consequences

- **Positive**:
  - Operators can browse the API interactively at `http://localhost:8080/swagger`.
  - External integrators can generate typed client bindings from the YAML spec.
  - Request/response types are now checked at compile time via generated Go types.
  - The gRPC handler remains the single source of truth for business logic.
- **Negative**:
  - `oapi-codegen` and `kin-openapi` are new Go module dependencies.
  - The generated file (`gen/go/oapi/vmafx_server_v1.gen.go`) must be
    regenerated when the spec changes
    (`oapi-codegen --config api/openapi/oapi-codegen.yaml api/openapi/vmafx-server-v1.yaml`).
  - Swagger UI requires CDN access; air-gapped deployments must proxy
    `unpkg.com/swagger-ui-dist@5.18.2`.
- **Neutral / follow-ups**:
  - Add `POST /v1/score` as an oapi-routed endpoint in a follow-up (currently
    served by the legacy handler; the adapter's `ScoreVideoPair` is wired but
    the legacy handler takes precedence due to registration order).
  - Consider OpenAPI request validation middleware (e.g. `kin-openapi/openapi3filter`)
    as a follow-up — not included here to keep the PR focused.
  - The `swagger-ui-dist` version pin (5.18.2) should be bumped deliberately;
    a Renovate rule may be added later.

## References

- ADR-0703: vmafx-server Go gRPC + HTTP service (parent ADR).
- oapi-codegen v2: <https://github.com/oapi-codegen/oapi-codegen>
- kin-openapi: <https://github.com/getkin/kin-openapi>
- OpenAPI 3.0.3 spec: <https://spec.openapis.org/oas/v3.0.3>
- swagger-ui-dist v5.18.2: <https://unpkg.com/swagger-ui-dist@5.18.2>
- PR #101 (this PR).
- req: "Add OpenAPI (Swagger) spec + generated REST handlers to vmafx-server.
  Currently has gRPC + HTTP transport but no formal REST contract."
