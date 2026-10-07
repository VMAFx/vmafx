<!-- markdownlint-disable MD013 MD060 -->
# ADR-2350: The VMAFx platform keeps its state in PostgreSQL, scales on queue depth and generates its platform surfaces from a definition

- **Status**: Proposed (open questions Q1 to Q8 below; the recommended options are written into the decision until they are answered)
- **Date**: 2026-10-07
- **Deciders**: maintainer
- **Tags**: cloud, k8s, controller, node, operator, helm, codegen, rc4, supply-chain, security

## Context

The distributed platform (controller, nodes, operator, scoring server;
[ADR-0709](0709-vmafx-phase4b-distributed-platform.md)) works on one machine
but cannot scale or recover by itself. On `origin/master` `18b04cf8b`:

| Area | Today | Evidence |
| --- | --- | --- |
| Job state | One controller replica owns an embedded SQLite queue; [ADR-1119](1119-golusoris-go-framework-adoption.md) rejected golusoris `jobs` (River on PostgreSQL) to keep it | `cmd/vmafx-controller/queue/queue.go`, `main.go:20-23,233`, [ADR-0711](0711-vmafx-controller-impl.md) |
| Node sessions | In the controller's memory; a restart makes every node register again | `cmd/vmafx-controller/nodes/registry.go:14` |
| Deployment | `replicas: 1`, `Recreate`, a ReadWriteOnce claim for the queue | `deploy/helm/vmafx/templates/controller.yaml:39-42,156-172`, [ADR-1589](1589-helm-controller-workload.md) |
| Inputs | Local paths, http(s) URLs or rclone remotes read on the node with an rclone configuration Secret; FUSE and eBPF modes need extra privileges | [ADR-0719](0719-vmafx-node-rclone-integration.md), [ADR-1526](1526-node-storage-streamed-inputs.md), `deploy/helm/vmafx/templates/node.yaml` |
| Results | A pooled score and a feature map in the queue row; nothing exported, no time series | `cmd/vmafx-controller/queue/schema.sql` |
| Scaling | Replica counts set by hand; no autoscaler object in the chart | `deploy/helm/vmafx/values.yaml` (`node.replicaCount`) |
| Platform surfaces | Written by hand: `proto/vmafx.proto` (buf) next to `cmd/vmafx-controller/proto/controller.proto` with its own protoc script; CRD types with a hand-written deepcopy file; the CRD YAML in two trees (`config/crd/bases/`, `deploy/helm/vmafx/crds/`); the `VmafxTenant` CRD with no Go type although it carries a controller-gen annotation; `deploy/helm/vmafx/values.schema.json`; no controller-gen run anywhere in the build | `cmd/vmafx-controller/proto/generate.sh`, `cmd/vmafx-operator/AGENTS.md` invariants 1 and 2 |
| Drift between surfaces | `VmafxJob` phases are Pending, Running, Succeeded and Failed; the controller also has `cancelled` | `api/vmafx/v1/vmafxjob_types.go`, `cmd/vmafx-controller/queue/queue.go` |

Decision Q-112 (2026-10-07) moves the cloud-native core into RC4 and replaces
ADR-1119's queue decision: state out of the processes, disposable and
horizontally scalable services, queue-driven scaling, events, every platform
surface generated from the API definition, and a standalone profile kept for
single-binary and development use (issue
[#2431](https://github.com/VMAFx/vmafx/issues/2431)).
[ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md) had put containers,
the Helm chart, the operator and the controller / node split into RC5; this
record moves the platform core into RC4 and leaves the GPU pool arbiter in
`libgpudispatch` ([#1253](https://github.com/VMAFx/vmafx/issues/1253)) in RC5.
The maintainer's direction was that high availability alone is not the aim:
every core cloud-native feature has to be in place before the quality work of
later releases (no-reference live scoring, generation loops, live monitors)
builds on it, and as much as possible is generated from the definition.

Constraints that shape the design:

- **The node API is an authenticated pull protocol.** A node registers with a
  token whose tenant owns the session
  ([ADR-1522](1522-controller-tenant-scoped-reads.md),
  [ADR-1563](1563-controller-node-role.md)), pulls work and reports results
  over gRPC, and decodes untrusted media with FFmpeg.
- **River executes a job only inside a River client.** `JobCompleteTx` refuses
  to run outside a worker and the client has no fetch-without-execute call
  ([Research-2351](../research/2351-cloud-native-platform-components.md)), so
  a node behind the gRPC API cannot take and complete a River job.
- **golusoris first** (ADR-1119): every element maps to a golusoris module or
  a named external component, and a missing golusoris capability is added to
  golusoris, not worked around here.
- **One generator** (HISS-19): the RC4 generator
  (`scripts/codegen/vmafx-api.py`, [ADR-1852](1852-vmafx-api-redesign.md); the
  proto and OpenAPI emitters of ADR-2044 in draft PR #2258).
- **Observability belongs to [#2430](https://github.com/VMAFx/vmafx/issues/2430)**:
  metrics, dashboards, alerts and the log bridge have one definition there.

Every version and licence below was checked against the upstream release or
documentation on 2026-10-07 ([Research-2351](../research/2351-cloud-native-platform-components.md)).

## Decision

We will run the controller as several stateless replicas over PostgreSQL, keep
node work in leased claims on the job table, put every asynchronous step on
River, cache through golusoris `cache/twotier`, keep inputs and outputs in
object storage, export results as signed OCI artifacts, publish job events as
CloudEvents through a transactional outbox, scale node pools on queue depth
with KEDA, and generate the platform's protobuf, CRDs, OpenAPI and Helm values
schema from a platform definition read by the RC4 generator. A standalone
profile runs the same code on SQLite with an in-process cache.

| Element | Provider | Checked version | Status |
| --- | --- | --- | --- |
| State store | PostgreSQL 18 through a CloudNativePG `Cluster`, or an external server; golusoris `db/pgx`, `db/sqlc`, `db/migrate` | CloudNativePG 1.30.1, PostgreSQL 18.6 | D1; packaging Q4 |
| Backups, point-in-time recovery | Barman Cloud plugin for CloudNativePG, object storage, workload identity | 0.15.1 | D1 |
| Node work | Leased claims on the job table | | D2, Q1 |
| Asynchronous steps, retries, periodic tasks | River through golusoris `jobs` | River 0.49.0 (golusoris pins 0.47.0) | D2 |
| Cache | golusoris `cache/twotier`: L1 `cache/memory` (otter v2), L2 Valkey through rueidis; Dragonfly tested as a drop-in | Valkey 9.1.2, rueidis 1.0.78 | D5 (maintainer answer, 2026-10-07) |
| Object storage | golusoris `storage` (S3 API; GCS and Azure Blob added there) | | D6, Q7 |
| Score time series | TimescaleDB hypertables, golusoris `db/timescale` | 2.30.2 | D7, Q3 |
| Artifacts | OCI 1.1 artifacts signed with Sigstore | image-spec 1.1.1, sigstore-go 1.3.0 | D8, Q6 |
| Events | golusoris `outbox` into River, CloudEvents on golusoris `pubsub/nats` or `pubsub/kafka`; webhooks | CloudEvents 1.0.2, NATS 2.15.0, Kafka 4.3.1 | D9, Q2 |
| Scaling | KEDA `ScaledObject` with the PostgreSQL scaler per node pool, scale to zero; KEDA for the scoring server | KEDA 2.21.0 | D10 |
| Placement | GPU device plugins or DRA `ResourceClaimTemplate`s; Node Feature Discovery labels | Kubernetes 1.37, NFD 0.19.0 | D10, Q5 |
| Idempotency | Per-tenant idempotency key on submission; golusoris `idempotency` for REST | | D11 |
| Leader election | River's own elector for River maintenance; golusoris `leader/k8s` (Lease) for the outbox drainer | | D11 |
| mTLS | cert-manager certificates and trust-manager bundles; golusoris `grpc` client-certificate checks and reload | cert-manager 1.21.2, trust-manager 0.25.0 | D12 |
| Secrets | External Secrets Operator writes Secrets, mounted as files, read by golusoris `secrets` | ESO 2.12.0 | D12 |
| Generation | Platform definition and RC4 generator emitters; buf, controller-gen, oapi-codegen downstream | buf 1.73.0, controller-tools 0.22.0 | D13 |
| Standalone profile | golusoris `db/sqlite`, River `riversqlite`, L1-only cache, local bucket, OCI image-layout directory | | D4 |
| Observability | #2430 | | D17 |

### D1. State and schema

- Every durable fact lives in PostgreSQL: jobs, attempts, node sessions,
  idempotency records, the outbox, River's tables, the artifact index and the
  score series. A replica's memory holds caches only; any replica can be
  killed at any time.
- Schema changes are golang-migrate files embedded in the controller
  (`vmafx-controller migrate up`), run by a Kubernetes Job that carries both
  the Helm and the GitOps hook annotations and is safe to run twice. A
  controller whose database is below the schema version it needs reports not
  ready and names both versions; it never starts on an old schema.
- Queries are sqlc-generated (golusoris `db/sqlc`). Every query keeps the
  tenant in its `WHERE` clause as today (ADR-1522), and row-level security adds
  a second wall: each transaction sets `vmafx.tenant_id` and the policies
  refuse other tenants' rows; maintenance runs under a separate role.
- PostgreSQL 18 is the chart's default; 16 and 17 are supported for external
  servers (TimescaleDB 2.30 supports 16 to 18; PostgreSQL 14 ends on
  2026-11-12).

### D2. Node work: leased claims, River for everything around them (Q1)

- The job row is the record. A node's `PullWork`, on any replica, claims the
  oldest pending job of its tenant that its backends can run with
  `SELECT … FOR UPDATE SKIP LOCKED`, opens attempt *n* (the fencing token),
  and writes the node session and `lease_expires_at`.
- Node sessions live in the database (token stored as SHA-256). `Heartbeat`,
  on any replica, extends the leases of the jobs the node lists, so a
  controller replica can die without the node registering again.
- A final `ReportResult` commits only when attempt and session match the
  running attempt, and writes the job's outbox event in the same transaction.
  A job therefore gets one terminal result and one completion event however
  often it was scored; a late report from a fenced attempt is refused, as
  `ErrNotAssigned` is today.
- An expired lease (node gone, network split) returns the job to pending with
  a delay from River's retry policy until the job's attempt limit, then fails
  it. The sweep is a River periodic job, so River's elector runs it once.
- A submission sends `NOTIFY` with the backend, so waiting `PullWork` long
  polls on every replica return at once; a short poll covers a lost
  notification.
- River runs every other step, each one retried with backoff and keyed as a
  unique job by job and step: staging inputs (presigning), ingesting the
  per-frame report into the series, rendering the report, exporting and
  signing artifacts, publishing events, delivering webhooks, retention and
  garbage collection.
- The node protocol keeps its RPCs; the attempt number and lease length are
  added fields within `vmafx.controller.v1`.

### D3. Data model

| Table | Holds |
| --- | --- |
| `jobs` | ID (UUIDv7), tenant, idempotency key (unique per tenant), kind, specification (`jsonb`: object references of the inputs and the generated `ScoreOptions`), required backend, priority, status (`pending`, `running`, `completed`, `failed`, `cancelled`), attempt and attempt limit, `available_at`, lease session and expiry, result summary (pooled scores, model digest, provenance digest), error, timestamps |
| `job_attempts` | One row per attempt: node, session, start, end, outcome, error |
| `node_sessions` | Session ID, tenant, node ID, token hash, capability (`jsonb`), last heartbeat, expiry |
| `artifacts` | Digest, tenant, job, kind, media type, repository, signature digest, time |
| `score_frames`, `score_windows` | Job, tenant, `observed_at`, frame index or window bounds, metric, value |
| golusoris `outbox`, River's tables, REST idempotency records | as their modules define them |

Tenants stay where [ADR-1519](1519-controller-tenant-registry.md) put them
(`VmafxTenant` resources or a file); their specification gains a storage
scope, a retention period and webhook endpoints. Objects live under
`tenants/<tenant>/inputs/…` and `tenants/<tenant>/jobs/<job>/…`; exported
artifacts under a per-tenant repository of the configured registry.

### D4. Standalone profile

`VMAFX_PROFILE=standalone` runs the same controller on one SQLite file
(golusoris `db/sqlite`), River on its `riversqlite` driver, an L1-only cache, a
local bucket directory, artifacts in an OCI image-layout directory, events
delivered to webhooks without a bus, and the series in an SQLite table. The
interfaces (`store`, `jobs.Client`, `twotier`, `storage.Bucket`, the artifact
and event publishers) are the same in both profiles, and one conformance
suite runs every interface test against both. The queries exist once per SQL
dialect (sqlc has a PostgreSQL and an SQLite engine); the conformance suite is
what keeps the two in step. River marks its SQLite driver experimental; the
standalone profile is for single-binary and development use and the suite
covers what it relies on.

### D5. Cache

golusoris `cache/twotier`: L1 in-process on otter v2, L2 on Valkey through
rueidis, singleflight on misses. Valkey (BSD-3-Clause) is the default L2, from
the official `valkey-io/valkey-helm` chart as an optional dependency or an
external endpoint. Dragonfly is a tested drop-in through the same client but
not the default: BUSL-1.1 until 2030-11-01 forbids its use as an in-memory
data store product or as a service, and that would pass to every operator who
hosts VMAFx; the documentation states those terms. The end-to-end suite runs
the L2 against both. The cache never holds the only copy of anything: it holds
digest-addressed model and artifact manifests, tenant registry snapshots,
REST idempotency responses and short-lived job status for polling clients
(`GetJob`, `StreamJobs`, the operator). Losing L2 slows reads and loses
nothing.

### D6. Object storage and inputs (Q7)

Inputs and outputs are object references (`s3://bucket/key`; `gs://` and
`az://` once golusoris `storage` has those backends). The controller resolves
a reference only inside the tenant's storage scope (the successor of the
scoring roots, [ADR-1577](1577-scoring-paths-per-tenant.md)) and gives the node
presigned URLs per attempt: GET for inputs, PUT for outputs. The node reads
and writes HTTPS and holds no storage credential; the controller signs with
workload identity. Large client uploads use presigned PUT or tus (golusoris
`storage/tus`). Local paths stay for the standalone profile. rclone remotes
stay as an opt-in node storage mode for backends without presigned URLs, off
by default because its FUSE and eBPF modes need privileges
([ADR-1593](1593-helm-node-fuse-and-ebpf.md)). The end-to-end suite uses SeaweedFS
(Apache-2.0); MinIO is archived and AGPL-3.0.

### D7. Score time series (Q3)

Per-frame and window scores go into `score_frames` and `score_windows`,
written by a River job from the per-frame report the node uploads. They are
ordinary tables, made hypertables when TimescaleDB is installed; the queries
use standard SQL (`date_bin`), so they run on both, and retention is a River
periodic job (`drop_chunks` on a hypertable, batched deletes otherwise). The
chart's default database uses the Apache-2.0 TimescaleDB edition from
CloudNativePG's extension image (PostgreSQL 18, Kubernetes 1.35, containerd
2.1). Compression and continuous aggregates are licensed under the Timescale
License and stay opt-in for operators who supply such an image; the
documentation states the TSL terms. The standalone profile keeps the series in
SQLite.

### D8. Artifacts (Q6)

Each finished job can be exported as an OCI 1.1 artifact: artifact type
`application/vnd.vmafx.result.v1+json`, the empty config, and layers
`application/vnd.vmafx.scores.v1+json`, `application/vnd.vmafx.report.v1+json`
and `application/vnd.vmafx.provenance.v1+json` (the WP5 provenance record).
A Sigstore bundle (`application/vnd.dev.sigstore.bundle.v0.3+json`) is attached
as a referrer, signed keyless with the workload's identity where Fulcio can
verify the cluster's issuer and with a KMS key reached through workload
identity otherwise. Models are artifacts of type
`application/vnd.vmafx.model.v1+json` and nodes pull them by digest. The
platform definition lists every media type, so constants and the reference
page are generated. The client is golusoris `container/registry`
(go-containerregistry), extended upstream with artifact push and pull and the
referrers API; its keychains give registry access through workload identity.
The standalone profile writes an OCI image-layout directory.

### D9. Events (Q2)

Every job state change writes an outbox row in its own transaction (golusoris
`outbox`). The drainer runs on one replica (golusoris `leader/k8s`) and turns
rows into River jobs; a River worker publishes a CloudEvent (structured JSON,
spec 1.0.2) on NATS JetStream by default or on Kafka. Types are
`dev.vmafx.job.{submitted,started,completed,failed,cancelled}.v1` and
`dev.vmafx.artifact.published.v1`; the subject is the job ID; the extension
`tenantid` names the tenant and `traceparent` carries the trace. Delivery is
at least once and consumers deduplicate on the event `id`. Webhooks are
per-tenant endpoints in the `VmafxTenant` specification, delivered by River
jobs with retries and an HMAC signature, in the CloudEvents HTTP binding. Event
types and their data schemas are in the platform definition.

### D10. Scaling and placement (Q5)

- Each node pool is a Deployment with a KEDA `ScaledObject` whose PostgreSQL
  scaler counts the pending jobs the pool can run, through a read-only role;
  `minReplicaCount: 0` scales the pool to zero when idle and out on the first
  submission. The query is generated with the schema.
- Scale-in drains: on SIGTERM a node stops pulling, finishes its attempts
  within the pool's grace period and releases what is left; a released attempt
  returns to pending without counting as a failure. The controller raises
  `controller.kubernetes.io/pod-deletion-cost` on node pods with running
  attempts, so idle pods go first (nodes get no right to patch pods).
- The scoring server scales with KEDA on CPU and in-flight requests (the
  latter a #2430 metric), never below one replica.
- Placement: pools request GPUs through device plugins by default; a pool can
  use a DRA `ResourceClaimTemplate` (`resource.k8s.io/v1`) instead. Node
  Feature Discovery labels select CPU pools by instruction set. No values file
  maps models or jobs to devices: a pool asks for one device of a class, the
  node discovers its devices at start, and `libgpudispatch` (RC5) arbitrates
  inside the pod.
- Kubernetes 1.35 is the minimum (1.34 ends on 2026-10-27); kind runs 1.35,
  1.36 and 1.37.

### D11. Resilience

- `SubmitJob` takes an idempotency key; the same key from the same tenant
  returns the first job's ID (unique index). REST takes `Idempotency-Key`
  through golusoris `idempotency` with a shared store; `VmafxJob` uses its
  resource UID.
- Retries back off through River; PodDisruptionBudgets cover controller,
  server and node pools; topology spread places controller and server replicas
  across nodes and zones; the controller runs two replicas by default with a
  rolling update.
- Readiness means database reachable and schema current; a missing L2 is
  reported but does not make a replica unready.
- Leader election only for singletons: River elects for its own maintenance
  and periodic jobs, golusoris `leader/k8s` for the outbox drainer.

### D12. Security

- Workload identity, no static keys, for object storage, registries,
  CloudNativePG backups and KMS.
- mTLS between every in-cluster service (controller, server, nodes, operator,
  MCP server) with cert-manager certificates from the chart's issuer or a given
  cluster issuer and trust-manager bundles; certificates are reloaded on
  rotation. The certificate authenticates the workload, the JWT the tenant
  (ADR-0794 unchanged).
- External Secrets for every credential workload identity cannot replace
  (database, Valkey, bus, registries without federation).
- The NetworkPolicies gain the database, cache, bus and registry egress; result
  artifacts are signed (D8) as the images already are.
- Nodes hold no storage or database credential (D2, D6).

### D13. Generation from a platform definition

A platform definition, `api/vmafx-platform.toml`, is read by the RC4
generator next to `core/api/vmafx.toml` and may refer to its enums, option
groups (`ScoreOptions`) and proto structs (`Provenance`). New tables:

| Table | Generates |
| --- | --- |
| `[[messages]]` | Protobuf messages, OpenAPI schemas, CRD spec and status types |
| `[[services]]` | gRPC services, the per-RPC role table ([ADR-1518](1518-controller-grpc-authorization.md)), REST mappings |
| `[[resources]]` | CRD Go types with kubebuilder markers (kind, scope, short names, printer columns, validation, subresources) |
| `[[events]]` | CloudEvents types, data JSON Schemas, Go constants, the reference page |
| `[[media_types]]` | OCI media type constants and their reference page |
| `[[config]]` | Each binary's environment keys (type, default, secret or not, documentation, Helm path): the Go key lists golusoris needs (`CompoundKeys`), the environment tables of the binary pages, the Helm environment mapping |
| `[[chart]]` | The rest of the Helm values tree, Kubernetes types referenced from the JSON schemas of the minimum supported minor |

Outputs and the tools that run after the generator:

- Protobuf under `proto/vmafx/` (controller, scoring service, event data),
  then one `buf generate` (configuration version 2) and
  `scripts/proto/postprocess_gen_go.py`. `cmd/vmafx-controller/proto/generate.sh`
  and `gen.go` are removed; `buf breaking` against master is part of the
  drift gate.
- CRD Go types under `api/vmafx/v1/` (including `VmafxTenant`), then
  controller-gen (pinned as a Go tool) writes the deepcopy file, the CRD YAML
  into `deploy/helm/vmafx/crds/` (the only tree; `config/crd/bases/` is
  removed) and the RBAC roles. A compatibility check refuses a removed field or
  narrower validation within `v1`.
- OpenAPI for the REST surfaces under `api/openapi/`, then oapi-codegen as
  today; the client generation of [#2321](https://github.com/VMAFx/vmafx/issues/2321)
  starts from these files.
- `deploy/helm/vmafx/values.schema.json` (JSON Schema draft-07 keywords, which
  Helm 3 and 4 both read), `values.yaml` with its comments, and
  `templates/_config.gen.tpl`.
- SQL migrations stay hand-written (an ordered, reviewed history); a test
  checks that every persisted message field has its column.

`test_vmafx_api_generated_current` and a CI step that reruns buf,
controller-gen and oapi-codegen fail on any difference, each shown failing on
a planted defect (a hand edit of a CRD, of the schema and of a generated
proto). The platform surfaces version like the scoring contract: additive
within `v1`, a breaking change as `v2` side by side.

### D14. Cluster prerequisites and what the chart installs (Q4)

Cluster-scoped operators are documented prerequisites with tested versions:
CloudNativePG and its Barman Cloud plugin, KEDA, cert-manager and
trust-manager, External Secrets Operator, Node Feature Discovery, and the GPU
device plugins or DRA drivers. The chart renders only the namespaced objects
that use them (`Cluster`, `ObjectStore`, `ScheduledBackup`, `ScaledObject`,
`TriggerAuthentication`, `Certificate`, `ExternalSecret`,
`ResourceClaimTemplate`), each behind a switch, and refuses to render one
whose API the cluster does not serve. Namespaced services may come as pinned
optional subcharts (Valkey, NATS) or as external endpoints; every external
mode (database, cache, bus, object storage, registry) works without them. No
subchart from the retired public Bitnami catalogue.

### D15. Node pools and the operator

Node pools stay Helm-rendered Deployments with their `ScaledObject`s, which
needs no operator and suits GitOps. The operator stays optional and owns the
declarative resources: a `VmafxJob` is submitted through the controller with
its UID as the idempotency key and mirrors the generated status (including
`Cancelled`); `VmafxModelTraining` keeps its role; `VmafxNode` reports pool
status.

### D16. Migration from the SQLite queue

- `vmafx-controller import-sqlite --from <file>` copies the old `jobs` rows:
  pending and running jobs become pending with attempt 0, finished jobs are
  kept, tenants are preserved, and a second run skips IDs already present. A
  controller pointed at a file of the old schema refuses to start and names
  that command.
- The chart drops `controller.persistence`; an optional pre-upgrade Job
  (`migration.importSqlite.existingClaim`) mounts the old claim and runs the
  import once. The controller defaults to two replicas with a rolling update.
- `VMAFX_DB_PATH` gives way to `VMAFX_DB_DSN` (or the CloudNativePG
  application Secret mounted as a file) and `VMAFX_PROFILE`; the migration page
  shows the values before and after, as the ADR-1589 move did.
- Nodes and clients change nothing; after the move their sessions survive a
  controller restart.

### D17. Observability boundary

Issue #2430 owns the metric definition, dashboards, alerts and the log bridge. The
platform's metrics are added through that one definition: queue depth by
backend and tenant, lease expiries, attempt outcomes, outbox lag, River queue
latency, cache hit ratio per tier, artifact exports, series ingest lag. Queue
gauges come from the store (one query, emitted by one replica); trace context
travels in River job metadata and in the CloudEvents `traceparent` extension.

## Open questions (each with the recommended option)

| # | Question | Options | Recommended, and why |
| --- | --- | --- | --- |
| Q1 | How do nodes get work, given that River completes a job only inside a River client? | (a) Leased claims on the job table, River for every step around them; (b) nodes as River workers with a database connection; (c) Temporal | (a): nodes keep the authenticated, tenant-bound gRPC protocol and hold no database credential while they decode untrusted media; fencing gives one result per job; River still owns retries, periodic work and every asynchronous step |
| Q2 | Default event bus | (a) NATS JetStream; (b) Kafka; (c) none by default, webhooks only | (a): one small server, an official chart, scale-to-zero friendly, a CloudEvents binding in sdk-go; Kafka stays supported through the same publisher and is tested |
| Q3 | TimescaleDB edition and managed databases without it | (a) Apache-2.0 edition by default, plain tables where the extension is missing, TSL features opt-in; (b) require a TSL image; (c) no TimescaleDB | (a): no licence passes to operators by default, and the platform runs on managed PostgreSQL that lacks the extension; compression stays available to those who accept the TSL |
| Q4 | CloudNativePG as a chart dependency or a prerequisite | (a) Prerequisite, the chart renders the `Cluster`; (b) subchart | (a): an operator is cluster-scoped (CRDs, one per cluster); a subchart conflicts with an existing installation and ties CRD upgrades to our releases. Same for KEDA, cert-manager, ESO, NFD |
| Q5 | GPU placement | (a) Device plugins by default, DRA per pool as an option; (b) DRA by default; (c) device plugins only | (a): no vendor's DRA driver allocates GPUs as a supported feature today (NVIDIA: not yet officially supported; Intel: beta; AMD: v1.0.1); the DRA path is built and tested so the default can move |
| Q6 | OCI client and media types | (a) Extend golusoris `container/registry` (go-containerregistry), own `vnd.vmafx` types, models included; (b) oras-go, own types; (c) (a) with the ModelPack types for models | (a): one OCI client in the fleet (HISS-19); the artifacts follow image-spec 1.1, so the oras CLI reads them; ModelPack is at v0.0.7 and built around language-model weights |
| Q7 | How nodes read inputs | (a) Presigned object URLs by default, rclone opt-in; (b) rclone by default; (c) remove rclone | (a): nodes hold no storage credential and need no FUSE or extra capability; rclone keeps backends without presigned URLs reachable |
| Q8 | Where node pools are defined | (a) Helm Deployments with `ScaledObject`s; (b) the operator renders pools from `VmafxNode` | (a): works without the optional operator and in plain GitOps; the operator can take pools over later without an API change |

## Alternatives considered

| Choice | Option | Pros | Cons | Outcome |
| --- | --- | --- | --- | --- |
| Node work (Q1) | Leased claims, River around them | Gateway and tenant model unchanged; fencing; no database access on GPU pods | One claim query to own beside River | Recommended |
| | Nodes as River workers | River's rescuer and retries apply directly | Database credentials on pods that decode untrusted media; no tenant-bound or remote nodes; the gRPC node API goes | Not recommended |
| | Temporal (golusoris `jobs/workflow`) | Activities with heartbeats on remote workers | A second cluster with its own persistence; Q-112 chose River | Not recommended |
| | River worker on a replica that holds the job while a node runs it | All work is a River job | A job is tied to one replica; another replica cannot complete it (`JobCompleteTx`) | Rejected |
| State store | PostgreSQL on CloudNativePG | Q-112; backups and recovery by the operator | A database to run | Chosen |
| | SQLite with a replication tool | No database server | One writer; no horizontal scale | Rejected |
| | Kubernetes resources as the queue | No new component | The API server is not a queue (object counts, watch fan-out, etcd size) | Rejected |
| Event bus (Q2) | NATS JetStream | Small, official chart, CloudEvents binding | Fewer enterprise connectors | Recommended |
| | Kafka | Where integrations already are | Heavy to run; no franz-go binding in sdk-go | Supported, not default |
| | Webhooks only | Nothing to run | No fan-out, no replay | Standalone profile |
| Series (Q3) | Apache edition, plain-table fallback, TSL opt-in | No licence passed on; managed PostgreSQL works | No compression by default | Recommended |
| | TSL image required | Compression, continuous aggregates | Licence passed to operators; managed PostgreSQL without the extension excluded | Not recommended |
| | Partitioned tables, no TimescaleDB | Nothing extra | Contradicts Q-112 | Rejected |
| Cluster operators (Q4) | Prerequisites | One installation per cluster, independent upgrades | One more install step | Recommended |
| | Subcharts | One `helm install` | CRD and version conflicts with existing installations | Not recommended |
| Placement (Q5) | Device plugins default, DRA opt-in | Works on every cluster today | Two code paths in the chart | Recommended |
| | DRA default | Structured device selection | Vendor drivers not supported for GPUs yet | Not recommended now |
| | Device plugins only | One path | Nothing ready when the drivers are | Not recommended |
| Artifacts (Q6) | golusoris `container/registry`, own types | One OCI client; keychains exist | Push and referrers to add upstream | Recommended |
| | oras-go | Artifact-first API | A second OCI client in the fleet | Not recommended |
| | ModelPack types for models | A shared standard | Pre-1.0 and language-model oriented | Revisit at its 1.0 |
| Inputs (Q7) | Presigned URLs, rclone opt-in | No credentials or privileges on nodes | URLs must outlive a long read (expiry set per attempt) | Recommended |
| | rclone by default | Many backends | Credentials and FUSE privileges on every node | Not recommended |
| | Remove rclone | Simplest node image | Backends without presigned URLs unreachable | Not recommended |
| Node pools (Q8) | Helm | No operator needed | Pools change with `helm upgrade` | Recommended |
| | Operator renders pools | Declarative pools as resources | Operator becomes required | Later |
| Definition | Separate platform definition, same generator | Platform changes never move the C ABI version | Two files | Chosen |
| | Extend `core/api/vmafx.toml` | One file | Every platform change bumps `abi_version` | Rejected |
| | `.proto` files and kubebuilder markers as sources | Usual tooling | Several sources, today's drift | Rejected |
| CRD YAML | Go types, then controller-gen | Standard schema and deepcopy | A second tool after the generator | Chosen |
| | The generator writes CRD YAML | One tool | Re-implements structural schemas and deepcopy | Rejected |
| L2 cache | Valkey default, Dragonfly tested | BSD-3-Clause | | Chosen (maintainer, 2026-10-07) |
| | Redis 8 | Original | RSALv2, SSPLv1 or AGPLv3 | Rejected |
| | Dragonfly default | Fast | BUSL-1.1 terms passed to operators | Rejected |
| mTLS | cert-manager certificates in the services | No data plane | Reload code in golusoris | Chosen (#2431) |
| | Service mesh | Transparent | Extra data plane; Linkerd has no open-source stable line | Rejected |
| Scaling | KEDA `ScaledObject` on a PostgreSQL query | Scale to zero, no metrics stack needed | A read-only database role | Chosen |
| | KEDA `ScaledJob` | A pod per job | Start-up and model load per job | Rejected |
| | HPA on a metric | Built in | No scale to zero; needs a metrics adapter | Rejected |
| Standalone SQL | sqlc for both dialects, one conformance suite | Native SQL per engine | Queries written twice | Chosen |
| | An ORM for portable SQL | One query text | Hides `SKIP LOCKED` and row-level security | Rejected |
| | Embedded PostgreSQL | One dialect | Large binary, slow start | Rejected |

## Consequences

- **Positive**: any controller replica or node can die mid-job and the job
  finishes once; nodes scale with demand and to zero; results, reports,
  provenance and models leave the cluster as signed, digest-addressed
  artifacts; integrations subscribe to events instead of polling; nodes need
  no storage credential and no FUSE privilege; the platform surfaces stop
  drifting because they are generated.
- **Negative**: a production installation needs PostgreSQL, the cluster
  operators of D14 and, optionally, Valkey and a bus; the queries exist in two
  dialects; golusoris needs the additions listed below before the dependent
  packages land; the environment contract changes (`VMAFX_DB_PATH`).
- **Follow-ups**:
  - When this record is accepted: ADR-1119, ADR-0711 and ADR-1589 become
    "Accepted (partially superseded by ADR-2350 …)" for the queue, the SQLite
    persistence and the one-replica workload, and ADR-2001 for its RC4 / RC5
    cloud-native rows; ADR-0719 and ADR-1526 as well if Q7 is answered (a).
    The statements of the old decision are rewritten in the same change:
    `cmd/vmafx-controller/AGENTS.md:49-51` (the prohibition to adopt
    golusoris `jobs`), `cmd/vmafx-controller/main.go:20-23,233`,
    `deploy/helm/vmafx/AGENTS.md:145-146`,
    `deploy/helm/vmafx/templates/controller.yaml:5-9,39`,
    `deploy/helm/vmafx/values.yaml:53-54,69`,
    `deploy/helm/vmafx/values.schema.json:33`,
    `docker/Dockerfile.controller:31,182`,
    `docs/development/k8s-deployment.md:162,181-182,436`,
    `docs/server/controller.md:31,46,64,204-213,246` and
    `docs/architecture/phase4b-distributed-platform.md:33`; until the store
    lands they describe the SQLite queue as the current, transitional state.
  - `cmd/vmafx-operator/AGENTS.md` invariant 5 (a hand-added `FinalScore`
    field) and `cmd/vmafx-controller/AGENTS.md` fx item 6 (hand-written
    bindings) no longer match the tree and go with the generation package.
  - golusoris additions, each filed upstream with a reproduction: River
    driver choice in `jobs` (SQLite) and the River update; an L1-only mode
    and cross-replica invalidation in `cache/twotier`; a shared store and a
    gRPC interceptor in `idempotency`; GCS and Azure Blob backends and
    presigned PUT in `storage`; artifact push, pull and referrers in
    `container/registry`; client-certificate verification and reload in
    `grpc` (keepalive is golusoris#589); CloudEvents in `pubsub` and
    `outbox`; edition detection in `db/timescale`; an SQLite source in
    `db/migrate`.
  - Operator documentation under `docs/cloud/` grows with each package
    (prerequisites and tested versions, profiles, backups and recovery,
    scaling, security, migration from the SQLite queue).
  - `docs/roadmap.md` and the bodies of #1251, #1252 and #1253 move the
    platform core from RC5 to RC4.

### Work packages

One pull request each, through the merge train, in this order. Each carries
failing-first tests, its operator documentation and, where it changes the
platform's behaviour, a kind end-to-end case and a standalone-profile case.

| # | Package | Depends on |
| --- | --- | --- |
| 0 | This record accepted, Research-2351, the status lines and the rewrite of the old decision's statements | the answers to Q1 to Q8 |
| 1 | The three SQLite files committed under `cmd/vmafx-controller/` by #2036 go, and the test that wrote them gets a temporary path (suspect: a test that builds the production graph without `VMAFX_DB_PATH`, so the default relative path lands in the package directory) | none |
| 2 | Platform definition and the protobuf emitter: controller and scoring services, one buf configuration, wire-identical output | 0; ADR-2044 emitters (#2258) on master |
| 3 | CRD emitter and controller-gen: one CRD tree, `VmafxTenant` Go type, generated deepcopy and RBAC, compatibility check | 2 |
| 4 | Configuration and chart emitters: Helm values schema, values file, environment mapping, `CompoundKeys`, environment tables | 2 |
| 5 | Store: schema, migrations Job, sqlc for PostgreSQL and SQLite, leased claims with fencing, sessions in the database, conformance suite | 0; golusoris `jobs` SQLite driver, `db/migrate` SQLite source |
| 6 | Controller on the store and River: lease sweep, notifications, `import-sqlite`, readiness on schema version, standalone profile | 5; the queue statistics interface of #2430's first package |
| 7 | Chart for high availability: CloudNativePG `Cluster` and backups or external database, two controller replicas, rolling update, PodDisruptionBudgets, topology spread, no claim; kind case: kill a controller and a node mid-job, the job completes once | 4, 6 |
| 8 | Idempotency keys and attempt fencing on the wire (`SubmitJob`, `ReportResult`, REST) | 2, 6; golusoris `idempotency` store |
| 9 | Object storage: object references, presigned URLs per attempt, tenant storage scope, uploads, SeaweedFS in the suite, rclone opt-in | 6, 8; golusoris `storage` presigned PUT (GCS and Azure Blob when they land) |
| 10 | Two-tier cache with Valkey and Dragonfly in the suite | 6; golusoris `cache/twotier` L1-only mode |
| 11 | Score series: ingestion, query RPCs, retention, edition handling | 9; answer Q3; golusoris `db/timescale` edition detection |
| 12 | Artifacts: export, signing, model pull by digest, media types from the definition | 9, 11; golusoris `container/registry` artifacts |
| 13 | Events and webhooks: outbox, CloudEvents on NATS and Kafka, event definitions | 6, 2; golusoris `pubsub` / `outbox` CloudEvents |
| 14 | Scaling and placement: KEDA for node pools and server, drain and deletion cost, DRA templates, NFD labels; kind case: scale out from zero and back | 7 |
| 15 | Security: mTLS with cert-manager and trust-manager, workload identity pages, External Secrets, NetworkPolicies | 7; golusoris `grpc` mTLS and reload |
| 16 | Operator: `VmafxJob` through the controller with idempotency and the generated status | 3, 8 |
| 17 | Final pass: GitOps rendering test, the complete `docs/cloud/` guide, the migration page, roadmap and issue text | 1 to 16 |

Packages 2 to 4 also cover the generated surfaces the planning audit found
written by hand (protobuf, CRDs including `VmafxTenant`, the duplicated CRD
trees, the Helm values schema). Bindings in further languages (Swift, Kotlin,
TypeScript) belong to the C API and stay with their 1.1 and 1.2 issues.

## Supply-chain impact

- **New runtime dependencies** (through golusoris modules, pinned in
  `go.mod`): River (MPL-2.0), pgx (MIT), rueidis (Apache-2.0), aws-sdk-go-v2
  (Apache-2.0), go-containerregistry (Apache-2.0), sigstore-go (Apache-2.0),
  nats.go (Apache-2.0) and franz-go (BSD-3-Clause). **Build and test**: sqlc
  (MIT), controller-tools (Apache-2.0), buf (Apache-2.0), SeaweedFS
  (Apache-2.0) in the suite.
- **Cluster prerequisites** with tested versions in `docs/cloud/`:
  CloudNativePG 1.30.1 and the Barman Cloud plugin 0.15.1, KEDA 2.21.0,
  cert-manager 1.21.2, trust-manager 0.25.0, External Secrets Operator 2.12.0,
  Node Feature Discovery 0.19.0, GPU device plugins or DRA drivers. Images
  (PostgreSQL with the TimescaleDB extension image, Valkey, NATS) and chart
  dependencies are pinned by version and digest.
- **Attack surface**: three network services (database, cache, bus) behind
  NetworkPolicies and TLS; nodes lose their storage credentials and, by
  default, the FUSE and eBPF privileges.

## References

- `Q` (Q-112, 2026-10-07): "Re-plan + build in RC4, all core cloud-native features: state out of the processes (Postgres via CNPG with backups/PITR, River queue, two-tier cache with Valkey/Redis L2, S3-compatible object storage, OCI artifact export via ORAS, TimescaleDB score time series); disposable horizontally scalable services; KEDA queue-driven scaling incl. scale-to-zero GPU nodes, DRA/NFD placement; outbox + CloudEvents on NATS/Kafka; CRDs/proto/OpenAPI/Helm schema generated from the RC4 definition, migrations as jobs; idempotency, retries, PDBs, topology spread; workload identity, mTLS, External Secrets; standalone SQLite profile kept. Supersedes ADR-1119's queue decision."
- Maintainer question on the cache tiers (2026-10-07), answered with the recommendation: L1 golusoris `cache/memory` on otter v2; L2 Valkey by default; Dragonfly a tested drop-in, not the default, because of BUSL-1.1; both tested in the end-to-end suite; the documentation states the Dragonfly terms.
- Issues [#2431](https://github.com/VMAFx/vmafx/issues/2431), [#2430](https://github.com/VMAFx/vmafx/issues/2430), [#2155](https://github.com/VMAFx/vmafx/issues/2155), [#2321](https://github.com/VMAFx/vmafx/issues/2321), [#1251](https://github.com/VMAFx/vmafx/issues/1251), [#1252](https://github.com/VMAFx/vmafx/issues/1252), [#1253](https://github.com/VMAFx/vmafx/issues/1253).
- [Research-2351](../research/2351-cloud-native-platform-components.md) (versions, licences and limits checked on 2026-10-07).
- [ADR-1119](1119-golusoris-go-framework-adoption.md), [ADR-0711](0711-vmafx-controller-impl.md), [ADR-1589](1589-helm-controller-workload.md), [ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md), [ADR-0709](0709-vmafx-phase4b-distributed-platform.md), [ADR-0719](0719-vmafx-node-rclone-integration.md), [ADR-1526](1526-node-storage-streamed-inputs.md), [ADR-1522](1522-controller-tenant-scoped-reads.md), [ADR-1519](1519-controller-tenant-registry.md), [ADR-1518](1518-controller-grpc-authorization.md), [ADR-1563](1563-controller-node-role.md), [ADR-1577](1577-scoring-paths-per-tenant.md), [ADR-1593](1593-helm-node-fuse-and-ebpf.md), [ADR-0794](0794-controller-multi-tenant-auth-gateway.md), [ADR-1852](1852-vmafx-api-redesign.md); ADR-2044 (draft PR #2258).
