<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — cmd/vmafx-controller

Go controller service, VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (queue + node API) and `VmafxScoring`
(direct scoring) on single port, plus HTTP `/healthz /readyz /metrics /v1/score`.

Per-package invariants for subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, queue, node registry, FIFO scheduler |
| [ADR-0961](../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |
| [ADR-0962](../../docs/adr/0962-controller-streamjobs-and-reaper-stop.md) | StreamJobs snapshot + reaper stop signal | controller / queue / nodes correctness |
| [ADR-1119](../../docs/adr/1119-golusoris-go-framework-adoption.md) | golusoris fx framework adoption | composition root, env contract, lifecycle ordering, auth injection |
| [ADR-2350](../../docs/adr/2350-cloud-native-platform.md) | cloud-native platform (replaces ADR-1119 queue decision) | Postgres store, leased node claims, River, stateless replicas, generated proto |
| [ADR-1518](../../docs/adr/1518-controller-grpc-authorization.md) | gRPC authorisation: per-method role table, deny by default | auth interceptors, `grpc_roles.go` |
| [ADR-1522](../../docs/adr/1522-controller-tenant-scoped-reads.md) | tenant-scoped job reads, tenant-bound node sessions | queue, nodes, scheduler, gRPC handlers |
| [ADR-1519](../../docs/adr/1519-controller-tenant-registry.md) | tenant registry from VmafxTenant resources or file | `auth/tenants.go`, `tenants/`, `tenant_config.go` |
| [ADR-1563](../../docs/adr/1563-controller-node-role.md) | `vmafx:node` only role on node API, nothing else | `grpc_roles.go`, `auth/policy.go`, `auth/tenants.go` |

## Protobuf bindings (ADR-1119) — GENERATED, never hand-written

`controllerv1` Go bindings at `gen/go/controller/{controller,controller_grpc}.pb.go`
generated from `proto/vmafx/controller/v1/controller.proto`, itself generated
from `api/vmafx-platform.toml` (ADR-2350 D13): `scripts/codegen/vmafx-api.py
--write`, then `scripts/codegen/proto_generate.py --write` (one `buf generate`).

- **NEVER hand-edit `.pb.go` files.** Hand-written stubs lacked `proto.Message`
  implementation (no `protoimpl`/`ProtoReflect`); `VmafxController` RPCs
  failed marshaling while unit tests passed. Guard:
  `cmd/vmafx-controller/wire_test.go` round-trips `SubmitJob`/`GetJob`/`StreamJobs`
  over `bufconn` `grpc.Server`. Edit `controller.proto`, regenerate, keep test green.
- Job-status enum: **top-level `JobStatus`** (not nested in `Job`); Go type:
  `controllerv1.JobStatus`, matching grpc_server/queue/scheduler. Nesting in
  `Job` generates `Job_Status`, breaking call sites.

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

## Invariants

### queue package

1. **PullWork rollback completeness (ADR-0961)**: 3-step rollback in
   `PullWork` (SQL UPDATE to `pending`, `runningSet` delete, FIFO prepend)
   atomic under `q.mu`. Do not early-return between
   `q.runningSet[matchID] = struct{}{}` and `getUnlocked` without updating
   `rollbackTopending`.
2. **`getUnlockedHook` test-only (ADR-0961)**: `getUnlockedHook` and
   `SetGetUnlockedHookForTest` prohibited in production. `ForTest` suffix
   = naming contract.
3. **runningSet / pendingFIFO consistent**: SQL job status changes mirror in
   `runningSet` and `pendingFIFO`. `reload()`: recovery on restart, not
   primary mechanism.
4. **`Queue.ListByTenant` contract (ADR-0962, ADR-1522)** (`queue/queue.go`):
   `ListByTenant(ctx, tenantID, statuses)` returns snapshot of one tenant's
   jobs, filtered by status strings. Empty `statuses` = all statuses.
   `tenant_id = ?` sits in SQL WHERE. No all-tenants read exists; never add
   one (old `ListAll` streamed every tenant's jobs). `StreamJobs` relies on contract.
5. **`ListByTenant` must include `tenant_id` in SELECT** (`queue/queue.go`):
   queries select `COALESCE(tenant_id,'')` into `job.TenantID` (`Job.TenantID` was `""`).
   Missing `tenant_id` broke `StreamJobs` tenant display. Schema additions must
   update both SELECT clauses and `rows.Scan`. Guard:
   `queue_listall_test.go:TestListByTenant_TenantIDRoundTrip`.
6. **Tenant-scoped `PullWork`, node-guarded `ReportResult` (ADR-1522)**:
   `findPendingMatch` skips other tenants' jobs. `ReportResult(ctx, Report)`
   UPDATE carries `AND assigned_node = ?`; zero rows -> `reportDecision`:
   terminal own/orphan job = idempotent nil; RUNNING job of same tenant with
   orphaned node (`Report.Orphaned`, no live session; ADR-1524 re-registered
   node) -> `adoptOrphan` compare-and-set on status + tenant, moves
   assignment; else `ErrNotAssigned`, nothing written, `runningSet` untouched.
   Never drop node guard or tenant compare. Partial reports: `MayReport`.
   Guards: `queue_tenant_test.go`, `grpc_tenant_test.go`.

7. **`Cancel` / `ReportResult` report the transition** (WP16, issue #2430):
   `(bool, error)`; true only when this call moved job to terminal state.
   Idempotent retry / already-terminal job -> false. Job metrics
   (`metrics.go::jobFinished`) count only true -> one count per job. Never
   count on nil error alone. Guard: `metrics_test.go::TestJobLifecycleMetrics`
   (repeat report + repeat cancel counted once).
8. **`Stats` = only all-tenant read, counts only** (`queue/stats.go`):
   per-tenant pending / running COUNT + MIN(created_at) and in-memory requeue
   totals (`requeued`, reasons `RequeueNodeLost` / `RequeueRestart` /
   `RequeueRollback` = `metricdef.RequeueReason` values). Returns no job row,
   no job content (ADR-1522 scopes job reads). Increment `requeued` under
   `q.mu` where a RUNNING job returns to PENDING (`RequeueNode`,
   `rollbackTopending`, `reload` count). Guard: `queue/stats_test.go`.
9. **`CancelledAmong` (ADR-1567)**: tenant + `status='cancelled'` in SQL
   WHERE, IDs bound (`repeatCommaQ`), input order kept, writes nothing.
   `Heartbeat` answers `cancel_job_ids` from it; `maxHeartbeatJobs` = 64
   (node slot limit) -> InvalidArgument above. Refused session -> ok=false,
   nothing named. Guards: `queue/cancelled_test.go`,
   `heartbeat_cancel_test.go`.

### scheduler package

- Invariants pending scheduler ADR.

### nodes package

1. **`nodes.NewRegistry` Start/Close lifecycle (ADR-1119)** (`nodes/registry.go`):
   `NewRegistry(log *slog.Logger)` takes no context, spawns no reaper. Reaper
   started by `StartDetached()` (fx `OnStart` in `provideNodeRegistry`),
   stopped by `Close()` (fx `OnStop`). Never `r.Start(ctx)` with the fx
   OnStart ctx: fx lets it expire after the start timeout (15 s) -> reaper
   stopped, silent nodes never evicted (`TestNodeRegistryReaperOutlivesStartContext`).
   Tests call `Close()` in `t.Cleanup`. `Close()` safe without `Start()`.
   Replaced eager `NewRegistry(ctx)` (ADR-0962).
2. **Eviction requeues** (`requeueEvictedNode` in `main.go`): registry
   eviction hook -> `queue.RequeueNode` returns the evicted node's RUNNING
   jobs to PENDING (FIFO front). Hook runs outside the registry lock; keep it
   (controller.proto promises the requeue).
3. **Sessions belong to one tenant (ADR-1522)**: `Register(name, tenantID,
   cap)` stores `Node.TenantID`; `ValidateSession` and `Heartbeat` compare
   token (constant time) and tenant (exact) in `sessionMatches`. Every session
   check takes caller tenant; no tenant-less variant.

### store package (ADR-2350, WP17; not wired yet)

PostgreSQL store replacing SQLite queue. `store/`: migrations
(`migrations/postgres/`, embedded, golusoris `db/migrate`), sqlc queries
(`queries/postgres/` -> generated `pgdb/`), Go API on top.

1. **Leased claims, attempt = fencing token** (Q-122): `Claim` = `FOR UPDATE
   SKIP LOCKED`, opens attempt n + `job_attempts` row. `Report` / `Release`
   / `ExtendLeases` UPDATE compares `attempt` AND `lease_session` AND
   `status = 'running'`. Never drop either compare: same session reclaiming
   after expiry is fenced only by `attempt`
   (`TestFencingRefusesAnOldAttemptOfTheSameSession`). Repeat of own ended
   attempt = no-op success only for outcomes in `reportRepeatable` /
   `releaseRepeatable`; report of released/expired attempt = `ErrFenced`.
2. **Tenant twice**: tenant in every WHERE + RLS (`FORCE ROW LEVEL
   SECURITY`, policies on `vmafx.tenant_id` / `vmafx.maintenance`). Tenant
   ops via `inTenant`, cross-tenant only via `inMaintenance` (expiry,
   counts). App role must not be superuser (superuser bypasses RLS); tests
   run as owner role `vmafx_app` (`TestRowLevelSecurityHidesOtherTenants`).
3. **Generated, never hand-edited**: `pgdb/*.go` = sqlc output + SPDX header.
   Edit SQL, then `python3 scripts/codegen/sqlc_generate.py --write` (sqlc
   pinned in `build-config.env`: `SQLC_VERSION` + per-platform SHA-256).
   Gate: Meson `test_sqlc_generated_current` (77 + reason without pinned
   binary). Schema change = new migration file + raise `SchemaVersion`.
4. **Bounded loops** (HISS-02): `MaxHeartbeatJobs` 64, `MaxBackends` 16,
   `MaxListJobs` 10000, `MaxExpiryBatch` 1000; larger input -> `ErrInvalid`.
5. **Tests need Docker** (golusoris `testutil/pg`; `postgres:18.6-alpine`,
   migrations also on `postgres:16.15-alpine`). No skip-on-missing-Docker
   added here; `-short` skips (testutil). `storetest.Image` / `OldestImage`
   stay `tag@sha256:<index digest>`: testutil/pg >= golusoris v0.13.0 fails
   test on undigested image. Tag bump = new digest, same edit.

### backend package (ADR-2350, WP17)

Seam between handlers and state: `backend.Backend`. `Postgres` = on
`store`; SQLite queue + registry behind same interface until SQLite store
lands. Wire protocol unchanged.

1. **Node ID = session ID** (Postgres): `Register` returns session UUID as
   node ID; `Report` / `MayReport` resolve attempt from session
   (`store.AttemptOf` / `RunningAttempt`), not from wire (wire fencing =
   WP8). Malformed node ID -> `ErrInvalidSession`; malformed job ID ->
   `ErrNotFound` (reads) / `ErrNotAssigned` (reports).
2. **Lease sweep = River periodic job** (`LeaseSweepKind`): leader inserts,
   any replica works it, `SKIP LOCKED` keeps sweeps apart. No
   `UniqueOpts.ByPeriod` (River refuses < 1 s; leader-only insert is
   enough). Requeues counted per process (`RecordSweep`).
3. **One-shot commands before fx** (`Command`): `migrate` (store + River
   migrations, then `CheckSchema`), `import-sqlite --from F
   [--empty-tenant T]` (read-only on F, refuses unmigrated DB, second run
   skips copied IDs). `VMAFX_DB_DSN` required. Exit 2 = usage.
4. **Backoff**: `ExponentialBackoff(base, max)`; golusoris `jobs` retry
   policy replaces it with the golusoris bump (same formula).
5. **Tests** (`go test ./cmd/vmafx-controller/backend/`, Docker): end to
   end, tenants apart, refusals, River sweep, import, commands.
   `store/storetest` = shared DB helper (tests only).
6. **River lifetime** (`store_wiring.go`, `riverRunner`): River started
   with own context, ended after `Stop`. Never pass fx `OnStart` ctx to
   `river.Start`: fx ends that ctx at start timeout (fxtest at start return) ->
   River stops fetching, notifier spins. Start retried in background
   (2 s, 900 tries); `/readyz` 503 until River runs (`postgresBackend.Ready`)
   -> replicas start before DB / migration Job without exiting. Guards:
   `TestRiverKeepsSweepingAfterTheStart`,
   `TestControllerStartedBeforeItsMigrationsWaitsUnready`.

### grpc server

1. **`protoStatusToQueue` / `queueStatusToProto` sync (ADR-0962)** (`grpc_server.go`):
   inverse helpers. New `Job.Status` requires updating both plus `queue.Status*`.
2. **`grpc_server_test.go` mock stream (ADR-0962)**: `mockStreamJobsServer`
   implements `grpc.ServerStream` (6 methods inline).
3. **Per-RPC role policy (ADR-1518)** (`grpc_roles.go`, `auth/policy.go`):
   `controllerMethodRoles()` names roles for every served method; auth
   interceptors authenticate then authorise in one function (`admitGRPC`).
   Unlisted method = refused for every caller, disabled-mode admin included.
   New RPC -> add entry in same PR; `TestEveryServedRPCHasARolePolicy` fails
   otherwise. Never chain separate role interceptor; never move role checks
   into handlers. `TestGRPCRolesEnforcedPerRPC` holds independent
   expectation table: change only together with ADR-0794/ADR-1518 role table.
   Node API = `auth.RoleNode` (`vmafx:node`) only, in no other entry; admin
   never on node API (ADR-1563). `devClaims` holds admin + node (disabled-mode
   nodes register). `tenantRoles` refuses `defaultRole: vmafx:node`; CRD
   offers node in `allowedRoles` only. Never re-add admin to node API.
4. **`auth/authtest` test-only**: RS256 issuer + JWKS server for tests.
   Import from `_test.go` files only.
5. **Tenant from context, once (ADR-1522)**: handlers read tenant via
   `callerTenant(ctx)` once, hand value down (queue query, registry, scheduler).
   Empty tenant -> `Unauthenticated`. Ownership refusals name no tenant
   (`AssertTenantOwns`).

### tenant registry (ADR-1519)

1. **One verification path** (`auth/middleware.go::verifyBearer`): HTTP and
   gRPC both call `verifyBearer`; registry mode -> `TenantRegistry.Resolve`,
   else global provider. Never add transport-specific token checks.
2. **Resolve contract** (`auth/tenants.go`): issuer picks candidates; each
   verifies with own JWKS/issuer/audience; match only when own `tenantClaim`
   value == own `tenantId`; exactly one match. Suspended -> PermissionDenied
   (403); stale set (age > `StaleFactor` x refresh) -> Unavailable (503).
   One snapshot (`atomic.Pointer`) per request; never reload mid-decision.
3. **Startup strict, refresh lenient**: `Load` all-or-nothing (controller
   start fails); `Reload` drops only invalid tenants and every copy of
   duplicate `tenantId`. Undecodable resource = entry with `Err`, refused by
   registry, never load failure. Mode exclusivity in `Config.validateMode`:
   registry excludes disabled and all five global provider settings.
4. **JWKS cache** (`auth/middleware.go`): keys used `jwksKeyMaxAge` (15 min)
   then refetched; on fetch failure kept until `jwksKeyHardMaxAge` (24 h);
   every fetch attempt sets cooldown; https->http redirect refused
   (`refuseDowngradeRedirect`). Registry: one signature check per cache
   (`matchTenant` + `verifiedOnce`), caches pruned in `publish`. gRPC
   Unauthenticated message fixed (`grpcAuthError`); reasons only logged.
5. **Sources** (`tenants/`): `FileSource` (YAML/JSON docs, lists) and
   `KubernetesSource` (dynamic client, namespace-scoped list, timeout).
   Strict spec decode (`DisallowUnknownFields`). `Refresher` Start/Close like
   `nodes.Registry`. Spec type = generated `vmafxv1.VmafxTenantSpec`
   (`api/vmafx/v1`, ADR-2350 D13); `auth/tenants.go` aliases `TenantSpec`,
   `TenantOIDC`, `TenantRBAC`, `TenantScoring` to it. New field ->
   `api/vmafx-platform.toml`, regenerate; never re-declare struct here. Guard:
   `auth/tenants_generated_type_test.go`.
6. **Guards**: `auth/tenants_internal_test.go` (once-per-cache, key age,
   redirect, prune), `auth/tenants_test.go` (mutation-checked: tenant-claim match,
   suspension, allowedRoles, staleness, defaultRole-in-allowed),
   `tenants/source_test.go`, `tenant_config_test.go`
   (`TestMisconfiguredTenantStopsStartup`, over-wire enforcement + reload).

### scoring roots (ADR-1577)

1. **Deny by default** (`scoring_scope.go`, `pkg/scoringscope`): no roots =
   every input refused (`PermissionDenied`, 403). Registry mode: roots from
   `TenantSpec.Scoring` (`TenantRegistry.ScoringRoots`); else
   `VMAFX_SCORING_ROOTS` with `{tenant}`; both = startup error. Tenant ID with
   `/`, `\`, `:`, `.`, `..` never substituted.
2. **Where checked**: `Score` / `POST /v1/score` resolve (symlinks) and score
   the real path; `SubmitJob` lexical; `PullWork` sets `Job.scoring_roots`
   (only there, never GetJob/StreamJobs). Guards: `scoring_scope_test.go`,
   `auth/tenants_scoring_test.go`, `pkg/scoringscope` tests.

### main / shutdown

0. **`--version` before fx (ADR-1589)**: `isVersionRequest` prints
   `pkg/version` and returns before `fx.New`; no config, no listeners. Image
   smoke test depends on it. Guard: `version_flag_test.go`.

1. **Shutdown ordering (ADR-1119)** (`main.go`): graceful shutdown owned by
   fx lifecycle. Order: gRPC `GracefulStop` → queue `Close` + node reaper stop
   → scorer `Close`. Replaced `observability.NewShutdownContext()` / `errgroup` /
   `runHTTP`/`runGRPC`. Guard: `TestStopOrder`.
2. **Default DB path never relative** (`db_path.go`, `provideJobQueue`):
   `db.path` unset -> `vmafx/vmafx-controller.db` under `$XDG_STATE_HOME` /
   `~/.local/state` (macOS, Windows: user config dir), dir 0700; unknown ->
   startup error naming `VMAFX_DB_PATH`. Never fall back to working dir:
   old relative default committed `vmafx-controller.db{,-shm,-wal}` (#2036).
   `.gitignore` lists them. Guard:
   `db_path_test.go::TestJobQueueDefaultIsNotTheWorkingDirectory`.

### observability

1. **Metrics from `metricdef` only** (`metrics.go`, WP16 / issue #2430):
   controller families = `metricdef.ByEmitter(metricdef.Controller)`. Event
   families via `observability.NewCounter/NewHistogram`
   (`newControllerMetrics`); queue + registry families read at scrape time
   via `observability.RegisterScraped` (`registerQueueCollector`, one
   `Stats` query per scrape, `observability.ScrapeTimeout`). No
   `prometheus.New*Vec` / GaugeFunc / promauto here. Guard:
   `metrics_test.go::TestControllerServesEveryFamilyItEmits`.

2. **OTel from `bootstrap.Base` + `bootstrap.HTTPTracing`, spans from golusoris and `grpc_server.go`**
   (`main.go::productionOptions`, ADR-0782 / ADR-1119): `bootstrap.HTTPTracing`
   traces `POST /v1/score` with `otelhttp`; gRPC server spans via `grpcmod.Module`
   `otelgrpc`; child span `vmafx.job.submit` via `observability.StartSpan`.
   Guards: `app_test.go::TestOTelWiredThroughBootstrap` locks no-op default,
   `vmafx-controller` / `pkg/version`; `TestGRPCHealthEmitsLinkedSpans` proves
   propagation (client/server share trace).
