<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — cmd/vmafx-controller

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Go controller service, VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (queue + node API) and `VmafxScoring`
(direct scoring) on single port, plus HTTP `/healthz /readyz /metrics /v1/score`.

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `backend/*.go`, `store_wiring*.go` | [backend](AGENTS.d/backend.md) | Session UUID = node ID; River periodic lease sweep with detached ctx; /readyz 503 until running. |
| `main.go`, `store_wiring.go`, `config_keys.gen.go`, `app_test.go` | [fx-composition](AGENTS.d/fx-composition.md) | Single productionOptions graph; generated compoundKeys; ProvideServerOptionFn JWT; R1 stop order. |
| `**/*.go` | [governing-adrs](AGENTS.d/governing-adrs.md) | Follow ADR-0711, ADR-1119, ADR-1518/1519/1522/1563, ADR-2350 controller contracts. |
| `grpc_*.go`, `auth/policy*.go`, `auth/grpc_interceptor*.go`, `auth/authtest/*.go` | [grpc-server](AGENTS.d/grpc-server.md) | Deny-by-default controllerMethodRoles; node API = vmafx:node only; callerTenant read once. |
| `main*.go`, `db_path*.go`, `version_flag_test.go` | [main-and-shutdown](AGENTS.d/main-and-shutdown.md) | Handle --version before fx.New; shutdown gRPC -> queue/reaper -> scorer; never relative DB path. |
| `nodes/*.go`, `main.go` | [nodes](AGENTS.d/nodes.md) | StartDetached/Close reaper lifecycle; eviction requeues RUNNING jobs; tenant-bound sessions. |
| `metrics*.go`, `main.go`, `app_test.go` | [observability](AGENTS.d/observability.md) | Metrics from metricdef + observability helpers only; OTel via bootstrap.Base + HTTPTracing. |
| `/gen/go/controller/*.go`, `/proto/vmafx/controller/v1/controller.proto`, `/api/vmafx-platform.toml`, `wire_test.go` | [protobuf-bindings](AGENTS.d/protobuf-bindings.md) | Never hand-edit .pb.go or proto; regenerate from api/vmafx-platform.toml; top-level JobStatus. |
| `queue/*.go`, `scheduler/*.go`, `heartbeat_cancel_test.go`, `grpc_tenant_test.go` | [queue](AGENTS.d/queue.md) | Atomic PullWork rollback; tenant-scoped ListByTenant/PullWork/CancelledAmong; transition bool. |
| `scoring_scope*.go`, `auth/tenants_scoring_test.go`, `/pkg/scoringscope/*.go` | [scoring-roots](AGENTS.d/scoring-roots.md) | Deny input paths without configured roots; symlink-resolved on score, lexical on SubmitJob. |
| `store/**/*.go`, `store/migrations/postgres/*.sql`, `store/queries/postgres/*.sql`, `internal/storetest/storetest.go` | [store](AGENTS.d/store.md) | Attempt + session fencing; WHERE + FORCE RLS tenant isolation; sqlc generated; digested pg image. |
| `auth/middleware*.go`, `auth/tenants*.go`, `tenants/*.go`, `tenant_config*.go` | [tenant-registry](AGENTS.d/tenant-registry.md) | Single verifyBearer path; strict startup Load, lenient Reload; generated VmafxTenantSpec. |
