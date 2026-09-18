<!-- markdownlint-disable MD060 -->
# AGENTS.md — cmd/vmafx-controller

Go controller service, VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (job queue + node API) and `VmafxScoring`
(direct scoring) on single port, plus HTTP `/healthz /readyz /metrics /v1/score`.

Per-package invariants for automated agents in this subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, in-memory queue, persistent node registry, FIFO scheduler |
| [ADR-0961](../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |
| [ADR-0962](../../docs/adr/0962-controller-streamjobs-and-reaper-stop.md) | StreamJobs snapshot + reaper stop signal | controller / queue / nodes correctness |
| [ADR-1119](../../docs/adr/1119-golusoris-go-framework-adoption.md) | golusoris fx framework adoption | composition root, env contract, lifecycle ordering, auth injection |

## Protobuf bindings (ADR-1119) — GENERATED, never hand-written

`controllerv1` Go bindings at `gen/go/controller/{controller,controller_grpc}.pb.go`
= **generated** from `cmd/vmafx-controller/proto/controller.proto` via
`cmd/vmafx-controller/proto/generate.sh` (`//go:generate ./generate.sh`).

- **NEVER hand-edit or hand-write these `.pb.go` files.** Hand-written stubs
  once shipped, did not implement `proto.Message` (no
  `protoimpl`/`ProtoReflect`), so every `VmafxController` RPC failed to
  marshal at runtime while enum-only unit tests still passed.
  `cmd/vmafx-controller/wire_test.go` = regression guard — round-trips
  `SubmitJob`/`GetJob`/`StreamJobs` over real `bufconn`
  `grpc.Server`. Change `controller.proto` -> regenerate, keep that test
  green; do not patch generated output by hand.
- Job-status enum = **top-level `JobStatus`** (not nested in `Job`), so
  generated Go type = `controllerv1.JobStatus`, matching
  grpc_server/queue/scheduler call sites. Moving it back inside `Job` would
  regenerate as `Job_Status`, break every call site.

## fx composition (ADR-1119)

Controller wired as `fx.New(...).Run()` over golusoris framework.
`main.go` supplies only vmafx domain providers + invokes; golusoris owns
config (koanf, `VMAFX_` prefix, `.` delimiter), structured slog logging, OTel,
HTTP stack (chi router + graceful `*http.Server`), gRPC server (OTel +
logging + panic-recovery interceptors), signal handling / graceful shutdown.

1. **`productionOptions(envReplace)` = single source of graph.** `main`
   and `app_test.go` both build from it; env-options `fx.Replace` =
   parameter (not embedded) because fx forbids replacing same type twice —
   binary passes `Watch:true`, tests `Watch:false`. Keep two in
   lockstep.

2. **SQLite job queue KEPT — do NOT adopt `golusoris.Jobs`.** `provideJobQueue`
   wraps embedded `modernc.org/sqlite` queue with `OnStop` `Close`.
   Single-binary queue = deliberate ADR-1119 decision; migrating it to
   river/Postgres explicitly out of scope.

3. **JWT auth injected via golusoris#269 (`ProvideServerOptionFn`).** Unary
   and stream auth interceptors wired through
   `grpc.ProvideServerOptionFn(func(mw *auth.Middleware) grpc.ServerOption{...})`
   into `group:"grpc.serveropts"` group — fx constructs `*auth.Middleware`,
   passes it straight into option constructor. Do NOT reintroduce
   package-level holder / lazy per-RPC lookup (pre-v0.6.0 `globalAuthMW`
   workaround for `#225`'s concrete-option API); v0.6.0's `ProvideServerOptionFn`
   = fx-native way. Do not build Middleware in composition root.

4. **Lazy-provider bind guards load-bearing.** fx providers are lazy:
   `fx.Invoke(func(_ *http.Server){})` and `fx.Invoke(func(_ *grpc.Server){})`
   force golusoris to construct + bind HTTP and gRPC listeners. Removing
   either drops that surface silently — `TestAppStartsAndStops` /
   `TestGRPCListenerBindsAndServes` guard them.

5. **Stop order (R1).** `fx.Invoke(func(_ *libvmaf.Scorer, _ queue.Queue, _ *nodes.Registry){})`
   registered AHEAD of gRPC service-registration
   invoke, so scorer/queue/registry OnStop hooks appended before gRPC
   server's. fx fires OnStop in reverse, so drain order = gRPC
   `GracefulStop` → queue `Close` + reaper stop → scorer `Close`. `TestStopOrder`
   pins it; do not reorder those invokes.

6. **gen/go/controller proto hand-written.** `gen/go/controller/*.pb.go`
   types do NOT implement protobuf-v2 reflection interface, so
   `VmafxController` RPCs cannot be marshaled over wire by standard gRPC
   codec. Over-the-wire tests use protoc-generated `VmafxScoring` service;
   in-process handler tests call controller methods directly.

## Invariants

### queue package

1. **PullWork rollback completeness (ADR-0961)**: three-step rollback in
   `PullWork` — SQL UPDATE to `pending`, `runningSet` delete, FIFO re-prepend —
   must remain atomic under `q.mu`. Do not add early-return path between
   `q.runningSet[matchID] = struct{}{}` and `getUnlocked` call without also
   updating rollback path in `rollbackTopending`.

2. **`getUnlockedHook` test-only (ADR-0961)**: `getUnlockedHook` field
   and `SetGetUnlockedHookForTest` method must not be called in production code
   paths. `ForTest` suffix = hard naming contract.

3. **runningSet / pendingFIFO always consistent**: every path that changes SQL
   job status must mirror change in `runningSet` and `pendingFIFO`.
   `reload()` function = sole recovery mechanism on controller restart;
   must remain last line of defence, not primary correctness mechanism.

4. **`Queue.ListAll` contract (ADR-0962)** (`queue/queue.go`):
   `ListAll(ctx, statuses)` returns point-in-time snapshot of all jobs,
   optionally filtered by provided status strings. Empty `statuses`
   slice means "all statuses." `StreamJobs` in `grpc_server.go` depends on
   this contract. Do not change semantics (e.g. change empty-slice
   meaning to "no jobs") without updating `StreamJobs` and its tests.

5. **`ListAll` must include `tenant_id` in its SELECT** (`queue/queue.go`):
   both SQL queries in `ListAll` (unfiltered and status-filtered paths)
   must select `COALESCE(tenant_id,'')` and scan it into `job.TenantID`.
   Omitting `tenant_id` from SELECT was confirmed regression: `Job.TenantID`
   always `""` in every `ListAll` result, made it impossible for
   `StreamJobs` to surface submitter's tenant. Any schema migration that
   adds new columns must also reflect in both SELECT clauses and
   matching `rows.Scan` call. See `queue_listall_test.go:TestListAll_TenantIDRoundTrip`
   as regression guard.

### scheduler package

- No additional invariants yet. Update this file when scheduler behaviour
  formalised in ADR.

### nodes package

1. **`nodes.NewRegistry` Start/Close lifecycle (ADR-1119)** (`nodes/registry.go`):
   `NewRegistry(log *slog.Logger)` does **not** take context and does **not**
   spawn reaper at construction. Reaper launched by `Start(ctx)`
   (idempotent; wired to fx `OnStart` hook in `provideNodeRegistry`) and
   stopped + awaited by `Close()` (idempotent; wired to fx `OnStop` hook).
   Reaper bound to `Close`-owned context, so no goroutine leaks past
   `Close()`. Every call site drives Start/Close via lifecycle; tests must
   `Close()` in `t.Cleanup` (may pass cancellable ctx to `Start` to stop
   it early). `Close()` safe to call without prior `Start()`. This replaced
   `NewRegistry(ctx)` signature (ADR-0962), which tied goroutine to
   caller context and spawned it eagerly.

### grpc server

1. **`protoStatusToQueue` / `queueStatusToProto` must stay in sync (ADR-0962)**
   (`grpc_server.go`): these two conversion helpers = inverses of each
   other. Adding new `Job.Status` enum value requires updating both
   functions and corresponding `queue.Status*` constant.

2. **`grpc_server_test.go` mock stream (ADR-0962)** (`grpc_server_test.go`):
   `mockStreamJobsServer` satisfies `grpc.ServerStream` explicitly (all six
   methods implemented inline). If `grpc.ServerStream` gains new methods in
   dependency bump, update mock accordingly — interface-assertion
   compile error surfaces it.

### main / shutdown

1. **Shutdown ordering (ADR-1119)** (`main.go`): graceful shutdown owned by
   golusoris' fx lifecycle, not hand-rolled signal context. Stop order
   enforced by R1 construction-ordering invoke (see "fx composition" §5):
   gRPC `GracefulStop` → queue `Close` + node-registry reaper stop → scorer
   `Close`. No longer an `observability.NewShutdownContext()` /
   `errgroup` / `runHTTP`/`runGRPC` path — those removed. `TestStopOrder`
   = regression guard.

### observability

1. **OTel comes from `bootstrap.Base` + `bootstrap.HTTPTracing`, spans from
   golusoris and `grpc_server.go`** (`main.go::productionOptions`, ADR-0782 /
   ADR-1119): `bootstrap.HTTPTracing` sits next to `golusoris.HTTP`, so
   `POST /v1/score` gets `otelhttp` server span; gRPC server spans come
   from `grpcmod.Module`'s `otelgrpc` handler (auth interceptors chain
   after it, so rejected RPCs traced too); `vmafx.job.submit` = child
   span `SubmitJob` opens via `observability.StartSpan`. Controller
   has no private OTel code — `app_test.go::TestOTelWiredThroughBootstrap`
   locks no-op default and `vmafx-controller` / `pkg/version`
   identity; `TestGRPCHealthEmitsLinkedSpans` = fleet's gRPC
   propagation proof (client span and server span share one trace).
