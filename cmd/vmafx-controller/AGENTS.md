<!-- markdownlint-disable MD060 -->
# AGENTS.md — cmd/vmafx-controller

Go controller service for the VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (job queue + node API) and `VmafxScoring`
(direct scoring) on a single port, plus HTTP `/healthz /readyz /metrics
/v1/score`.

Per-package invariants for automated agents working in this subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, in-memory queue, persistent node registry, FIFO scheduler |
| [ADR-0961](../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |
| [ADR-0962](../../docs/adr/0962-controller-streamjobs-and-reaper-stop.md) | StreamJobs snapshot + reaper stop signal | controller / queue / nodes correctness |
| [ADR-1119](../../docs/adr/1119-golusoris-go-framework-adoption.md) | golusoris fx framework adoption | composition root, env contract, lifecycle ordering, auth injection |

## Protobuf bindings (ADR-1119) — GENERATED, never hand-written

The `controllerv1` Go bindings at `gen/go/controller/{controller,controller_grpc}.pb.go`
are **generated** from `cmd/vmafx-controller/proto/controller.proto` via
`cmd/vmafx-controller/proto/generate.sh` (`//go:generate ./generate.sh`).

- **NEVER hand-edit or hand-write these `.pb.go` files.** Hand-written stubs once
  shipped that did not implement `proto.Message` (no `protoimpl`/`ProtoReflect`),
  so every `VmafxController` RPC failed to marshal at runtime while the enum-only
  unit tests still passed. `cmd/vmafx-controller/wire_test.go` is the regression
  guard — it round-trips `SubmitJob`/`GetJob`/`StreamJobs` over a real `bufconn`
  `grpc.Server`. If you change `controller.proto`, regenerate and keep that test
  green; do not patch the generated output by hand.
- The job-status enum is **top-level `JobStatus`** (not nested in `Job`) so the
  generated Go type is `controllerv1.JobStatus`, matching the
  grpc_server/queue/scheduler call sites. Moving it back inside `Job` would
  regenerate as `Job_Status` and break every call site.

## fx composition (ADR-1119)

The controller is wired as an `fx.New(...).Run()` over the golusoris framework.
`main.go` supplies only the vmafx domain providers + invokes; golusoris owns
config (koanf, `VMAFX_` prefix, `.` delimiter), structured slog logging, OTel,
the HTTP stack (chi router + graceful `*http.Server`), the gRPC server (OTel +
logging + panic-recovery interceptors), and signal handling / graceful shutdown.

1. **`productionOptions(envReplace)` is the single source of the graph.** `main`
   and `app_test.go` both build from it; the env-options `fx.Replace` is a
   parameter (not embedded) because fx forbids replacing the same type twice —
   the binary passes `Watch:true`, the tests `Watch:false`. Keep the two in
   lockstep.

2. **SQLite job queue is KEPT — do NOT adopt `golusoris.Jobs`.** `provideJobQueue`
   wraps the embedded `modernc.org/sqlite` queue with an `OnStop` `Close`. The
   single-binary queue is a deliberate ADR-1119 decision; migrating it to
   river/Postgres is explicitly out of scope.

3. **JWT auth injected via golusoris#269 (`ProvideServerOptionFn`).** The unary
   and stream auth interceptors are wired through
   `grpc.ProvideServerOptionFn(func(mw *auth.Middleware) grpc.ServerOption{...})`
   into the `group:"grpc.serveropts"` group — fx constructs the `*auth.Middleware`
   and passes it straight into the option constructor. Do NOT reintroduce a
   package-level holder / lazy per-RPC lookup (the pre-v0.6.0 `globalAuthMW`
   workaround for `#225`'s concrete-option API); v0.6.0's `ProvideServerOptionFn`
   is the fx-native way. Do not build the Middleware in the composition root.

4. **Lazy-provider bind guards are load-bearing.** fx providers are lazy:
   `fx.Invoke(func(_ *http.Server){})` and `fx.Invoke(func(_ *grpc.Server){})`
   force golusoris to construct + bind its HTTP and gRPC listeners. Removing
   either drops that surface silently — `TestAppStartsAndStops` /
   `TestGRPCListenerBindsAndServes` guard them.

5. **Stop order (R1).** `fx.Invoke(func(_ *libvmaf.Scorer, _ queue.Queue, _
   *nodes.Registry){})` is registered AHEAD of the gRPC service-registration
   invoke so the scorer/queue/registry OnStop hooks are appended before the gRPC
   server's. fx fires OnStop in reverse, so the drain order is gRPC
   `GracefulStop` → queue `Close` + reaper stop → scorer `Close`. `TestStopOrder`
   pins it; do not reorder those invokes.

6. **gen/go/controller proto is hand-written.** `gen/go/controller/*.pb.go`
   types do NOT implement the protobuf-v2 reflection interface, so
   `VmafxController` RPCs cannot be marshaled over the wire by the standard gRPC
   codec. Over-the-wire tests use the protoc-generated `VmafxScoring` service;
   in-process handler tests call the controller methods directly.

## Invariants

### queue package

1. **PullWork rollback completeness (ADR-0961)**: The three-step rollback in
   `PullWork` — SQL UPDATE to `pending`, `runningSet` delete, FIFO re-prepend —
   must remain atomic under `q.mu`.  Do not add any early-return path between
   `q.runningSet[matchID] = struct{}{}` and the `getUnlocked` call without also
   updating the rollback path in `rollbackTopending`.

2. **`getUnlockedHook` is test-only (ADR-0961)**: The `getUnlockedHook` field
   and `SetGetUnlockedHookForTest` method must not be called in production code
   paths.  The `ForTest` suffix is a hard naming contract.

3. **runningSet / pendingFIFO always consistent**: Every path that changes SQL
   job status must mirror the change in `runningSet` and `pendingFIFO`.  The
   `reload()` function is the sole recovery mechanism on controller restart and
   must remain the last line of defence, not the primary correctness mechanism.

4. **`Queue.ListAll` contract (ADR-0962)** (`queue/queue.go`):
   `ListAll(ctx, statuses)` returns a point-in-time snapshot of all jobs,
   optionally filtered by the provided status strings.  An empty `statuses`
   slice means "all statuses."  `StreamJobs` in `grpc_server.go` depends on
   this contract.  Do not change the semantics (e.g. change empty-slice
   meaning to "no jobs") without updating `StreamJobs` and its tests.

5. **`ListAll` must include `tenant_id` in its SELECT** (`queue/queue.go`):
   Both SQL queries in `ListAll` (the unfiltered and the status-filtered paths)
   must select `COALESCE(tenant_id,'')` and scan it into `job.TenantID`.
   Omitting `tenant_id` from the SELECT was a confirmed regression: `Job.TenantID`
   was always `""` in every `ListAll` result, which made it impossible for
   `StreamJobs` to surface the submitter's tenant.  Any schema migration that
   adds new columns must also be reflected in both SELECT clauses and the
   matching `rows.Scan` call.  See `queue_listall_test.go:TestListAll_TenantIDRoundTrip`
   as the regression guard.

### scheduler package

- No additional invariants yet.  Update this file when scheduler behaviour is
  formalised in an ADR.

### nodes package

1. **`nodes.NewRegistry` Start/Close lifecycle (ADR-1119)** (`nodes/registry.go`):
   `NewRegistry(log *slog.Logger)` does **not** take a context and does **not**
   spawn the reaper at construction. The reaper is launched by `Start(ctx)`
   (idempotent; wired to the fx `OnStart` hook in `provideNodeRegistry`) and
   stopped + awaited by `Close()` (idempotent; wired to the fx `OnStop` hook).
   The reaper is bound to a `Close`-owned context, so no goroutine leaks past
   `Close()`. Every call site drives Start/Close via the lifecycle; tests must
   `Close()` in a `t.Cleanup` (and may pass a cancellable ctx to `Start` to stop
   it early). `Close()` is safe to call without a prior `Start()`. This replaced
   the `NewRegistry(ctx)` signature (ADR-0962) that tied the goroutine to a
   caller context and spawned it eagerly.

### grpc server

1. **`protoStatusToQueue` / `queueStatusToProto` must stay in sync (ADR-0962)**
   (`grpc_server.go`): these two conversion helpers are inverses of each
   other.  Adding a new `Job.Status` enum value requires updating both
   functions and the corresponding `queue.Status*` constant.

2. **`grpc_server_test.go` mock stream (ADR-0962)** (`grpc_server_test.go`):
   `mockStreamJobsServer` satisfies `grpc.ServerStream` explicitly (all six
   methods implemented inline).  If `grpc.ServerStream` gains new methods in
   a dependency bump, update the mock accordingly — an interface-assertion
   compile error will surface it.

### main / shutdown

1. **Shutdown ordering (ADR-1119)** (`main.go`): graceful shutdown is owned by
   golusoris' fx lifecycle, not a hand-rolled signal context. The stop order is
   enforced by the R1 construction-ordering invoke (see "fx composition" §5):
   gRPC `GracefulStop` → queue `Close` + node-registry reaper stop → scorer
   `Close`. There is no longer an `observability.NewShutdownContext()` /
   `errgroup` / `runHTTP`/`runGRPC` path — those were removed. `TestStopOrder`
   is the regression guard.
