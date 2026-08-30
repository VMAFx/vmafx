- **`vmafx-controller` migrated onto the golusoris fx framework** (ADR-1119,
  Phase-1 PR-2). The hand-rolled composition root (`flag.Parse` +
  `observability.NewLogger`/`InitOTel`/`NewShutdownContext` + `errgroup` +
  bespoke `runHTTP`/`runGRPC` listeners + a hand-built `grpc.NewServer` with
  recovery interceptors) is replaced by an `fx.New(...).Run()` over golusoris
  modules: `golusoris.Core` (config + structured slog), `otel.Module`,
  `golusoris.HTTP` (chi router + graceful `*http.Server`), and `grpc.Module`
  (gRPC server with OTel + logging + panic-recovery interceptors baked in), via
  the shared `internal/app/bootstrap.Base`. The domain logic — the embedded
  SQLite job queue, the node registry, the FIFO scheduler, the JWT auth
  middleware, and both gRPC service implementations — is unchanged.
- **Embedded SQLite job queue KEPT (deliberate, ADR-1119).** The controller is
  **not** migrated onto `golusoris.Jobs` (river/Postgres). The single-binary
  `modernc.org/sqlite` queue is the controller's zero-dependency operability
  story; only its construction moves into an fx provider with an `OnStop`
  `Close` hook.
- **JWT auth interceptors injected via golusoris#269 (`ProvideServerOptionFn`).**
  The unary + stream auth interceptors are wired into the golusoris gRPC server
  through `grpc.ProvideServerOptionFn(func(mw *auth.Middleware) grpc.ServerOption{…})`
  (the `group:"grpc.serveropts"` group the `grpc.Module` consumes) — fx
  constructs the `*auth.Middleware` and feeds it straight into the option
  constructor, so tenant isolation is enforced on every RPC after the framework's
  OTel/logging/recovery interceptors. This replaces the earlier `#225`
  concrete-option + package-level `globalAuthMW` holder workaround (removed): the
  per-RPC nil-check and the lazy holder are gone now that v0.6.0 exposes the
  fx-dependent constructor variant.
- **Node-registry reaper bound to the fx lifecycle.** `nodes.NewRegistry` no
  longer spawns a goroutine at construction or ties it to a caller context; the
  reaper is launched by `Start(ctx)` (an fx `OnStart` hook) and stopped + awaited
  by `Close()` (an fx `OnStop` hook), eliminating the goroutine-leak risk (same
  pattern the `vmafx-node` `FeedbackClient` adopted). Stop order at shutdown:
  gRPC `GracefulStop` → node-registry reaper stop + queue `Close` → scorer
  `Close` (pinned by `TestStopOrder`).
- **Breaking — env-var / listen-address contract.** The controller previously
  read `VMAFX_PORT` / `VMAFX_GRPC_PORT` as bare port numbers (`8080` / `50051`).
  It now reads `VMAFX_HTTP_ADDR` (koanf `http.addr`, default `:8080`) and
  `VMAFX_GRPC_LISTEN` (koanf `grpc.listen`, default `:9090`), which take a full
  listen address. The golusoris-native defaults apply — the legacy wire ports
  are **not** carried. The pre-fx auth/JWKS CLI flags are removed; auth is
  configured via `VMAFX_AUTH_DISABLED`, `VMAFX_JWKS_ENDPOINT`,
  `VMAFX_AUTH_ISSUER`, `VMAFX_AUTH_AUDIENCE`, `VMAFX_AUTH_TENANT_CLAIM`
  (CompoundKey `auth.tenant_claim`), and `VMAFX_AUTH_ROLES_CLAIM` (CompoundKey
  `auth.roles_claim`). See [`docs/usage/env-vars.md`](../docs/usage/env-vars.md).
- **Fixed — controllerv1 protobuf types now generated, not hand-written.** The
  `gen/go/controller/{controller,controller_grpc}.pb.go` bindings were
  hand-written and did **not** implement `proto.Message` (no
  `protoimpl`/`ProtoReflect`), so every `VmafxController` RPC failed to marshal
  at runtime — the control plane was non-functional over the wire even though
  the enum-only unit tests passed. They are now regenerated from
  `controller.proto` via `cmd/vmafx-controller/proto/generate.sh`
  (`//go:generate`), and the job-status enum is promoted to a top-level
  `JobStatus` (wire-compatible: field number + values unchanged) to match the
  call sites. New `cmd/vmafx-controller/wire_test.go` stands up a real
  `grpc.Server` over an in-process `bufconn` and round-trips `SubmitJob`,
  `GetJob`, and the `StreamJobs` server-stream so a regression to
  non-marshalable types fails loudly in CI instead of in production.
- **Built on golusoris v0.5.0.** golusoris#225 (`grpc.ProvideServerOption`),
  #227, and #234 are all in the pinned `v0.5.0` tag, so the controller compiles
  and runs against the shared go.mod pin with no interim shims.
  ([ADR-1119](../docs/adr/1119-golusoris-go-framework-adoption.md))
