- `vmafx-server`: formal OpenAPI 3.0.3 REST contract (`api/openapi/vmafx-server-v1.yaml`),
  oapi-codegen-generated Go stubs, REST adapter delegating to the gRPC handler,
  and Swagger UI at `/swagger` (spec at `/swagger/spec.json`).
  New endpoints: `GET /v1/health`, `GET /v1/ready`; legacy `/healthz` and `/readyz`
  aliases preserved. `VMAFX_SWAGGER_TRY_IT_OUT=1` enables try-it-out in the UI.
  ADR-0797. (PR #101)
