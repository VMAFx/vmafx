- **`vmafx-server` migrated onto the golusoris fx framework** (ADR-1119,
  Phase-1 PR-1). The hand-rolled composition root (`signal.NotifyContext` +
  `errgroup`, bespoke `*http.Server`/`grpc.NewServer` lifecycles, custom OTel
  init + logger) is replaced by an `fx.New(...).Run()` over golusoris modules:
  `golusoris.Core` (config + structured slog logging), `otel.Module`,
  `golusoris.HTTP` (chi router + graceful `*http.Server`), and the golusoris
  `grpc.Module` (OTel + logging + recovery interceptors baked in). The libvmaf
  cgo scorer, Prometheus `/metrics` exposition, OpenAPI REST adapter, Swagger
  UI, and the shared concurrency limiter are unchanged.
- **Breaking — server env-var contract.** The listen configuration moves from
  the bare-port `VMAFX_PORT` / `VMAFX_GRPC_PORT` to golusoris' full-address
  sub-keys `VMAFX_HTTP_ADDR` (default `:8080`) and `VMAFX_GRPC_LISTEN` (default
  `:9090`). Values are now full listen addresses (`:8080`), not bare port
  numbers, and the default gRPC address changed from `:50051` to `:9090`. The
  `--port` / `--grpc-port` CLI flags are removed (config is env-only). Update
  deployment manifests; see `docs/usage/env-vars.md` and `docs/server/grpc.md`.
