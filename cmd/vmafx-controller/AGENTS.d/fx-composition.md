---
paths:
  - cmd/vmafx-controller/main.go
  - cmd/vmafx-controller/store_wiring.go
  - cmd/vmafx-controller/config_keys.gen.go
  - cmd/vmafx-controller/app_test.go
invariant: Single productionOptions graph; generated compoundKeys; ProvideServerOptionFn JWT; R1 stop order.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# fx composition root and lifecycle (ADR-1119, ADR-2350)

## fx composition (ADR-1119)

Controller wired via `fx.New(...).Run()` over golusoris framework.
`main.go` supplies vmafx providers + invokes; golusoris owns config (koanf,
`VMAFX_` prefix, `.` delimiter), structured slog logging, OTel, HTTP stack
(`bootstrap.HTTP` = golusoris router + server modules: chi router + graceful `*http.Server`), gRPC server
(OTel + logging + panic-recovery interceptors), shutdown.

1. **`productionOptions(envReplace)` = single graph source.** `main` and
   `app_test.go` build from it. `fx.Replace` parameterised (fx forbids duplicate
   type replacement): binary passes `Watch:true`, tests `Watch:false`.
2. **Backend chosen by `VMAFX_STORE_BACKEND`** (`store_wiring.go`,
   ADR-2350): `sqlite` (default) = `provideJobQueue` + registry + scheduler
   behind `backend.Legacy`, one replica, transitional (no new features);
   `postgres` = `store` + River lease sweep behind `backend.Postgres`, any
   replica count, no DB credentials on nodes (Q-122). Standalone profile =
   same store on SQLite + `riversqlite` (waits golusoris#620/#621). Store
   keys: generated `compoundKeys` (`config_keys.gen.go`, from `[[config]]`
   of `api/vmafx-platform.toml`, ADR-2350 D13) feed `controllerEnvOptions`;
   tests build from `controllerEnvOptions()` (no second key list). New key ->
   `[[config]]` entry, regenerate; no hand list here.
3. **JWT auth injected via golusoris#269 (`ProvideServerOptionFn`).** Interceptors
   wired via `grpc.ProvideServerOptionFn(func(mw *auth.Middleware) grpc.ServerOption{...})`
   into `group:"grpc.serveropts"`. fx injects `*auth.Middleware`. Do NOT reintroduce
   package holder / per-RPC lookup (`globalAuthMW` `#225`). v0.6.0 native way.
   Do not construct Middleware in composition root.
4. **Lazy-provider bind guards load-bearing.** `fx.Invoke(func(_ *http.Server){})`
   and `fx.Invoke(func(_ *grpc.Server){})` force binding HTTP and gRPC listeners.
   Tested in `TestAppStartsAndStops` and `TestGRPCListenerBindsAndServes`.
5. **Stop order (R1).** `fx.Invoke(func(_ *libvmaf.Scorer, _ backend.Backend){})`
   registered AHEAD of gRPC registration; backend OnStop hooks (SQLite:
   queue `Close` + reaper stop; Postgres: River `Stop` + pool `Close`) run
   after gRPC `GracefulStop`, scorer `Close` last. Guard: `TestStopOrder`.
6. **gen/go/controller = buf output** (`protoc-gen-go`, `protoimpl`
   present) from `api/vmafx-platform.toml` (ADR-2350 D13). Never hand-edit
   proto or `.pb.go`; change the definition, run both generators. Guards:
   `wire_test.go` (`TestControllerProtoMarshalsOverWire`, `VmafxController`
   over `bufconn`), `test_proto_generated_current`, `buf breaking` (WIRE_JSON).
