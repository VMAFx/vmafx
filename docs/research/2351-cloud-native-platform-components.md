<!-- markdownlint-disable MD013 MD060 -->
# Research-2351: components of the cloud-native platform, their versions, licences and limits

- **Status**: Active
- **Workstream**: [ADR-2350](../adr/2350-cloud-native-platform.md) (issue [#2431](https://github.com/VMAFx/vmafx/issues/2431); observability is [#2430](https://github.com/VMAFx/vmafx/issues/2430))
- **Last updated**: 2026-10-07

## Question

Which components can carry the cloud-native target of #2431 (state out of the
processes, queue-driven scaling, artifacts in registries, events, generated
surfaces, a standalone profile), at which current version, under which
licence, and with which limits that change the design? Every row below was
read from the upstream release, licence file or documentation on 2026-10-07,
not from memory. Rows marked *unverified* could not be confirmed that day.

## Sources

Release pages are `https://github.com/<repository>/releases`; the licence is
the repository's licence file unless the row says otherwise.

| Component | Repository | Latest stable (date) | Licence |
| --- | --- | --- | --- |
| CloudNativePG operator | `cloudnative-pg/cloudnative-pg` | v1.30.1 (2026-09-23); chart `cloudnative-pg` 0.29.1 | Apache-2.0 |
| Barman Cloud plugin for CloudNativePG | `cloudnative-pg/plugin-barman-cloud` | v0.15.1 (2026-09-30) | Apache-2.0 |
| PostgreSQL | postgresql.org `versions.json` | 18.6 (2026-08-13); 14 is end of life on 2026-11-12 | PostgreSQL |
| TimescaleDB | `timescale/timescaledb` | 2.30.2 (2026-09-29), PostgreSQL 16 to 18 | Apache-2.0 outside `tsl/`, Timescale License (TSL) in `tsl/` |
| River | `riverqueue/river` | v0.49.0 (2026-10-05) | MPL-2.0 |
| Valkey | `valkey-io/valkey` | 9.1.2 (2026-09-01); chart `valkey-io/valkey-helm` 0.12.0 | BSD-3-Clause |
| Dragonfly | `dragonflydb/dragonfly` | v2.0.0 (2026-09-16) | BUSL-1.1, change date 2030-11-01 to Apache-2.0 |
| Redis | `redis/redis` | 8.10.2 (2026-09-17) | RSALv2, SSPLv1 or AGPLv3 (8.x); BSD-3-Clause up to 7.2 |
| rueidis | `redis/rueidis` | v1.0.78 (2026-09-15) | Apache-2.0 |
| otter | `maypok86/otter` | v2.3.0 (2025-12-22) | Apache-2.0 |
| KEDA | `kedacore/keda` | v2.21.0 (2026-09-23) | Apache-2.0 |
| Kubernetes | `kubernetes/kubernetes` | 1.37 (1.37.0 on 2026-08-26); 1.34 is end of life on 2026-10-27 | Apache-2.0 |
| GPU DRA driver (NVIDIA GPUs) | `kubernetes-sigs/dra-driver-nvidia-gpu` | v0.5.0 (2026-08-19) | Apache-2.0 |
| GPU DRA driver (Intel GPUs) | `intel/intel-resource-drivers-for-kubernetes` | gpu-v0.12.1 (2026-09-29) | Apache-2.0 |
| GPU DRA driver (AMD GPUs) | `ROCm/k8s-gpu-dra-driver` | v1.0.1 (2026-07-22) | Apache-2.0 |
| GPU device plugins | `NVIDIA/k8s-device-plugin`, `intel/intel-device-plugins-for-kubernetes`, `ROCm/k8s-device-plugin` | v0.20.1, v0.37.1, v1.31.0.11 | Apache-2.0 |
| Node Feature Discovery | `kubernetes-sigs/node-feature-discovery` | v0.19.0 (2026-07-10) | Apache-2.0 |
| OCI image specification | `opencontainers/image-spec` | v1.1.1 (2025-03-03) | Apache-2.0 |
| oras-go, oras CLI | `oras-project/oras-go`, `oras-project/oras` | v2.6.2, v1.3.4 | Apache-2.0 |
| go-containerregistry | `google/go-containerregistry` | v0.22.1 (2026-09-04), the version golusoris `container/registry` pins | Apache-2.0 |
| cosign, sigstore-go | `sigstore/cosign`, `sigstore/sigstore-go` | v3.1.3, v1.3.0 | Apache-2.0 |
| ModelPack model specification | `modelpack/model-spec` | tag v0.0.7 | Apache-2.0 |
| cert-manager, trust-manager | `cert-manager/cert-manager`, `cert-manager/trust-manager` | v1.21.2, v0.25.0 | Apache-2.0 |
| External Secrets Operator | `external-secrets/external-secrets` | v2.12.0 (2026-10-06) | Apache-2.0 |
| NATS server, nats.go | `nats-io/nats-server`, `nats-io/nats.go` | v2.15.0, v1.54.0; chart `nats` 2.15.0 | Apache-2.0 |
| Apache Kafka, franz-go, Strimzi | downloads.apache.org, `twmb/franz-go`, `strimzi/strimzi-kafka-operator` | 4.3.1, v1.22.1, 1.2.0 | Apache-2.0, BSD-3-Clause, Apache-2.0 |
| CloudEvents | `cloudevents/spec`, `cloudevents/sdk-go` | spec 1.0.2, sdk-go v2.16.2 | Apache-2.0 |
| golang-migrate, sqlc, pgx | `golang-migrate/migrate`, `sqlc-dev/sqlc`, `jackc/pgx` | v4.20.1, v1.31.1, v5.11.0 | MIT |
| buf, connect-go, grpc-go | `bufbuild/buf`, `connectrpc/connect-go`, `grpc/grpc-go` | v1.73.0, v1.21.0, v1.84.0 | Apache-2.0 |
| controller-tools (controller-gen) | `kubernetes-sigs/controller-tools` | v0.22.0 (2026-09-02) | Apache-2.0 |
| Helm | `helm/helm` | 4.3.0 and 3.22.0 | Apache-2.0 |
| kind, kuttl | `kubernetes-sigs/kind`, `kudobuilder/kuttl` | v0.33.0, v0.27.0 | Apache-2.0 |
| S3-compatible test servers | `seaweedfs/seaweedfs`, `versity/versitygw` | 4.48, 1.8.0 | Apache-2.0 |
| MinIO | `minio/minio` | last release 2025-10-16; repository archived | AGPL-3.0 |
| tusd | `tus/tusd` | v2.10.1 | MIT |
| Harbor | `goharbor/harbor` | v2.15.3 (2026-10-06) | Apache-2.0 |

The golusoris modules were read at `golusoris/golusoris` `origin/main`
`9dac7d0` (tag v0.12.0 is the version `go.mod` pins); River's behaviour was
read in its v0.47.0 source, the version golusoris pins.

## Findings

### Licences that pass conditions on to every operator

- **TimescaleDB.** Hypertables, `drop_chunks` and `time_bucket` are Apache-2.0.
  Compression (the columnstore), continuous aggregates, retention policies
  (`add_retention_policy`) and background jobs are TSL. TSL section 2.2
  forbids offering the TSL code to third parties "to provide time-series
  database functions or operations, other than as part of Your Value Added
  Products or Services", and a value-added service may not give its users
  direct database access (section 3.10). The extension image CloudNativePG
  publishes (`cloudnative-pg/postgres-extensions-containers`,
  `timescaledb-oss`) is the Apache-2.0 edition; a TSL build means the
  `timescale/timescaledb-ha` image as a custom `imageName`. golusoris
  `db/timescale` calls `EnableCompression`, `AddCompressionPolicy` and
  `SetRetention`, which are TSL functions. Managed PostgreSQL offerings differ
  in whether they offer the extension at all (not checked per provider).
- **Dragonfly.** BUSL-1.1 until 2030-11-01. The additional use grant allows
  use "only as part of your own product or service, provided it is not an
  in-memory data store product or service" and not "as a Service".
- **Redis 8.** RSALv2, SSPLv1 or AGPLv3 at the user's choice; Valkey is the
  BSD-3-Clause line.
- **MinIO.** AGPL-3.0 and archived; not a test dependency to add. SeaweedFS
  and versitygw are Apache-2.0 S3-compatible servers.
- **River Pro** (workflows, sequences, concurrency limits, durable periodic
  jobs) is commercial; open-source River has unique jobs, periodic jobs,
  retries with backoff, leader election and resumable jobs.

### Limits that change the design

- **River runs a job only inside a River client.** `JobCompleteTx` refuses to
  run outside a worker ("client not found in context, can only work within a
  River worker", `job_complete_tx.go` in v0.47.0), and the public client has
  no fetch-without-execute call (`client.go`: `Insert*`, `Job{Get,List,Cancel,
  Retry,Update,Delete}`, `Queue*`). A process that is not a River client
  (a remote, tenant-bound node behind the gRPC node API) cannot take and
  complete a River job.
- **River on SQLite exists but is experimental** (`riverdriver/riversqlite`,
  since v0.23.0; v0.40.0 added a pseudo listen/notify table for SQLite).
- **GPU allocation through DRA is not production-ready for any vendor.** The
  NVIDIA driver's README calls GPU allocation "not yet officially supported"
  (only ComputeDomains are); the Intel driver calls itself "beta /
  non-production"; the AMD driver is at v1.0.1. The Kubernetes side is stable
  (`DynamicResourceAllocation` gate stable in 1.34, `resource.k8s.io/v1`;
  prioritized lists and admin access stable in 1.36). Device plugins are
  stable and published for all three vendors.
- **Support windows.** CloudNativePG 1.30 supports Kubernetes 1.34 to 1.36
  and is tested on 1.37; KEDA 2.21 is tested on 1.34 to 1.36 (N-2 policy);
  the repository's kind job pins kind v0.33.0 and kubectl v1.37.0
  (`.github/workflows/e2e-k8s.yml`). Kubernetes 1.34 reaches end of life on
  2026-10-27.
- **CloudNativePG extensions from images** (`.spec.postgresql.extensions`)
  need PostgreSQL 18, Kubernetes 1.35 (image volumes on by default from 1.35)
  and containerd 2.1 or CRI-O 1.31.
- **CloudNativePG backups.** The in-tree `barmanObjectStore` is deprecated
  since 1.26; the Barman Cloud plugin is the supported path and authenticates
  with IRSA, GKE Workload Identity or Azure managed identity.
- **CloudEvents for Kafka with franz-go.** sdk-go has NATS, JetStream and two
  Kafka bindings (sarama, confluent), none for franz-go, which golusoris
  `pubsub/kafka` uses.
- **Helm values schemas.** Helm 3.22 and 4.3 both validate with
  `santhosh-tekuri/jsonschema/v6`; keywords of JSON Schema draft-07 are read
  by both lines and by older Helm 3 releases.
- **Linkerd** publishes no open-source stable releases since February 2024;
  mTLS from cert-manager certificates needs no mesh.
- **Bitnami charts and images** left the public catalogue on 2025-09-29 (old
  images moved to `bitnamilegacy`); a chart dependency on them would stop
  receiving updates.

### golusoris: what exists and what is missing

| Module | Has | Missing for #2431 |
| --- | --- | --- |
| `jobs` | River client over `pgxpool`, `Register`, 25 attempts and 30 s timeout by default, River UI | A driver choice (`riversqlite`) for the standalone profile; River is two minors behind (v0.47.0 against v0.49.0) |
| `outbox` | Transactional `Add(ctx, tx, …)`, leader-gated drainer into River jobs, golang-migrate files | SQLite; a CloudEvents envelope |
| `pubsub/nats`, `pubsub/kafka` | JetStream and franz-go clients | CloudEvents binary and structured mode for both |
| `cache/twotier` | L1 otter, L2 rueidis, singleflight, prefix invalidation | An L1-only mode (the constructor requires a `rueidis.Client`); invalidation of other replicas' L1 |
| `idempotency` | HTTP middleware for `Idempotency-Key`, in-memory store | A shared store (Postgres or Redis protocol); a gRPC interceptor |
| `storage` | `Bucket` interface, local and S3 (aws-sdk-go-v2 default credential chain, so IRSA and EKS Pod Identity), presigned GET | GCS and Azure Blob backends (GCS is marked planned), presigned PUT |
| `container/registry` | go-containerregistry client: resolve, manifest, tags, copy; keychains are injectable | Pushing and pulling arbitrary OCI artifacts (artifact type, own layers), referrers |
| `grpc` | Server TLS from a certificate file pair | Client-certificate verification (mTLS), certificate reload on rotation, configurable keepalive (open issue golusoris#589) |
| `db/timescale` | Hypertable, compression and retention helpers | Edition detection, so TSL-only calls fail with a named error on the Apache edition |
| `db/migrate` | golang-migrate on pgx v5 | A SQLite source for the standalone profile |
| `leader` | Kubernetes Lease and Postgres advisory-lock electors | nothing |
| `secrets` | Environment, file and static backends | nothing (External Secrets Operator writes Secrets that mount as files) |
| `k8s/operator` | controller-runtime manager under fx | nothing |

## Alternatives explored

- **Temporal** (golusoris `jobs/workflow`): long activities on remote workers
  with heartbeats fit the node model, but it adds a server cluster with its
  own persistence next to PostgreSQL; ADR-2350 lists it as rejected.
- **oras-go** next to go-containerregistry: both write OCI 1.1 artifacts;
  carrying two OCI clients in one fleet contradicts HISS-19.
- **Service meshes for mTLS:** Istio ambient (Apache-2.0, 1.31.1) works but
  adds a data plane; Linkerd has no open-source stable line.

## Open questions

Not confirmed on 2026-10-07: River's minimum PostgreSQL version; Kafka 4.x
being KRaft-only; which JSON Schema draft the Helm documentation states;
referrers API support in Harbor, GHCR, ECR, GAR, Docker Hub and Zot (ACR
supports it except for customer-managed-key registries); `cosign sign` on an
arbitrary artifact by digest (documented, not run); Azure federated workload
identity in the Barman Cloud plugin by that name; the default Kubernetes
versions of the managed offerings; the AMD DRA driver's production status.

## Related

- [ADR-2350](../adr/2350-cloud-native-platform.md), [ADR-1119](../adr/1119-golusoris-go-framework-adoption.md),
  [ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md),
  ADR-2044 (option groups and the scoring contract, draft PR #2258)
- Issues #2431 (this work), #2430 (observability), #2155 (scoring API contract)
