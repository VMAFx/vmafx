<!-- markdownlint-disable MD013 MD060 -->
# vmafx-operator — Kubernetes Operator

`vmafx-operator` is a [kubebuilder](https://kubebuilder.io/) v4 /
controller-runtime v0.25 Kubernetes operator that watches `VmafxJob`,
`VmafxNode`, and `VmafxModelTraining` custom resources and reconciles their
Pod + status sub-resources.

The operator is built per [ADR-0714](../adr/0714-vmafx-operator-skeleton.md)
and runs as a Kubernetes `Deployment` inside the cluster.

## Quick start (in-cluster)

1. Build the operator image.

    ```bash
    docker build -f docker/Dockerfile.operator -t vmafx-operator:dev .
    ```

2. Load it into the cluster, or push it to your registry.

    ```bash
    kind load docker-image vmafx-operator:dev
    ```

3. Deploy with the Helm chart.

    ```bash
    helm upgrade --install vmafx deploy/helm/vmafx/ \
        --set operator.enabled=true \
        --set operator.image.tag=dev
    ```

## Configuration (12-factor env vars)

Runtime configuration is environment-only. `--version` is the sole process
switch and exits before the Kubernetes manager starts.

<!-- BEGIN GENERATED: vmafx-api environment vmafx-operator (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_CONTROLLER_TLS` | `controller.tls` | bool | `false` |  | Dial the controller with TLS (system roots unless `VMAFX_CONTROLLER_CA_FILE` is set). |
| `VMAFX_CONTROLLER_CA_FILE` | `controller.ca_file` | path | system roots |  | PEM bundle that verifies the controller certificate; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_SERVER_NAME` | `controller.server_name` | string | host of the address |  | TLS server name override; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_TOKEN_FILE` | `controller.token_file` | path | _(unset)_ | `operator.controllerToken.secretName` | File holding the bearer token for the controller, read again on every call; an expired JWT is not sent. Not together with `VMAFX_CONTROLLER_TOKEN`. |
| `VMAFX_CONTROLLER_TOKEN` | `controller.token` | string | _(unset)_ |  | Secret. Bearer token for the controller given inline; not together with `VMAFX_CONTROLLER_TOKEN_FILE`. |
| `VMAFX_OPERATOR_METRICS_ADDR` | `operator.metrics_addr` | `host:port` | `:8080` | set by the chart | Bind address of the Prometheus metrics endpoint; `0` disables it. |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `operator.health_probe_addr` | `host:port` | `:8081` | set by the chart | Bind address of `/healthz` and `/readyz`. |
| `VMAFX_OPERATOR_LEADER_ELECTION` | `operator.leader_election` | bool | `false` | `operator.leaderElect` | Leader election; `true` for several replicas. |
| `VMAFX_OPERATOR_LEADER_ELECTION_ID` | `operator.leader_election_id` | string | `vmafx-operator.vmafx.dev` |  | Lease name of the leader election. |
| `VMAFX_OPERATOR_GRACEFUL_SHUTDOWN` | `operator.graceful_shutdown` | duration | `30s` |  | Graceful-shutdown timeout of the manager. |
| `VMAFX_OPERATOR_WEBHOOK_PORT` | `operator.webhook_port` | integer | `0` |  | Admission-webhook port; `0` disables the webhooks. |
| `VMAFX_OPERATOR_WEBHOOK_HOST` | `operator.webhook_host` | host | all interfaces |  | Admission-webhook bind host. |
| `VMAFX_CONTROLLER_GRPC_ADDR` | read directly | `host:port` | `vmafx-controller.<namespace>.svc.cluster.local:9090` | `controller.enabled`, `controller.grpcPort` | gRPC address of the controller, used by the `VmafxJob` reconciler. |
| `VMAFX_CONTROLLER_HTTP_ADDR` | read directly | URL | `http://vmafx-controller.<namespace>.svc.cluster.local:8080` | `controller.enabled`, `controller.httpPort` | HTTP address of the controller with its scheme, used by the `VmafxNode` health probe (`/healthz` is appended). |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` | `operator.logLevel` | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-operator -->

See the [full environment variable reference](../usage/env-vars.md) for the
complete cross-surface table.

## Authenticating to the controller

With auth on, the controller refuses a `GetJob` without a token, and the
`VmafxJob` reconciler then logs `Failed to poll controller for job status`
on every pass and never moves the phase. Give the operator a token
([ADR-1569](../adr/1569-operator-controller-auth.md)):

- The token is a JWT the controller accepts ([Auth gateway](auth.md)): its
  tenant claim names the tenant whose jobs the `VmafxJob` resources track
  (`GetJob` reads only the caller's tenant), and its roles include
  `vmafx:reader`.
- Put it in a file and set `VMAFX_CONTROLLER_TOKEN_FILE`. The operator reads
  the file on every `GetJob`, so a token that something rewrites (a Secret
  the kubelet updates, a sidecar that renews it from the identity provider)
  applies without a restart. A JWT whose `exp` has passed is not sent; the
  poll fails with `controller token file <path> holds a token that expired at
  <time>; whatever writes it did not refresh it`.
- With `VMAFX_CONTROLLER_TLS=true` the token never travels over plaintext.

The variables are the ones `vmafx-node` reads for its own controller client
(`pkg/controllerclient`), and the operator refuses to start on a combination
it cannot honour (a token file and an inline token, a CA file without TLS).

## Health probes

| Path | Port | Purpose |
| --- | --- | --- |
| `/healthz` | `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | Liveness — returns `200 OK` when the manager is running. |
| `/readyz` | `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | Readiness — returns `200 OK` when the cache has synced. |

## Prometheus metrics

Standard controller-runtime metrics are exposed at
`VMAFX_OPERATOR_METRICS_ADDR/metrics`.  Add a `ServiceMonitor` resource to
scrape them with Prometheus Operator.

## Leader election

In a multi-replica deployment set `VMAFX_OPERATOR_LEADER_ELECTION=true`. The
operator uses a `Lease` resource named `vmafx-operator.vmafx.dev` in the
operator's namespace for the lock.

## Reconcilers

| Resource | What the reconciler does |
| --- | --- |
| `VmafxJob` | Sets the phase to `Pending`, then polls the controller's `GetJob` RPC and maps the remote status (`PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`) onto the resource phase, score and timestamps. Running jobs are requeued; terminal phases are not. |
| `VmafxNode` | Probes the controller's `/healthz` every 30 s and marks the node unhealthy when the probe fails or its last heartbeat is older than 60 s. It never overwrites `status.lastHeartbeat`. |
| `VmafxModelTraining` | Sets the phase to `Initializing`, polls the sidecar trainer's `/status`, mirrors the sample count and last checkpoint into the status, and emits a `CheckpointWritten` event when a checkpoint advances. Requeues every 60 s. |

## Admission webhooks

The operator serves validating webhooks for `VmafxJob` and `VmafxNode` when
`VMAFX_OPERATOR_WEBHOOK_PORT` is set to a port other than `0`.

| Resource | Rule |
| --- | --- |
| `VmafxJob` | `spec.reference` and `spec.distorted` must be well-formed rclone URIs. The same rule applies on create and update. |
| `VmafxNode` | `spec.gpuVendor` must be one of `nvidia`, `amd`, `intel`, `cpu`. |

## Custom resource definitions

| CRD | Group | Kind |
| --- | --- | --- |
| VmafxJob | `vmafx.dev/v1` | Job submission and lifecycle tracking |
| VmafxNode | `vmafx.dev/v1` | Worker node registration and capability |
| VmafxModelTraining | `vmafx.dev/v1` | Sidecar training run lifecycle |
| VmafxTenant | `vmafx.dev/v1` | Tenant OIDC and role settings; read and enforced by the controller, not reconciled by the operator (see [auth](auth.md#tenant-registry)) |

Install CRDs from the Helm chart (enabled by default) or manually:

```bash
kubectl apply -f deploy/helm/vmafx/crds/
```

The manifests and their schemas are in `deploy/helm/vmafx/crds/`. A minimal
`VmafxJob` looks like this (see the CRD for every field):

```yaml
apiVersion: vmafx.dev/v1
kind: VmafxJob
metadata:
  name: example
spec:
  reference: s3://bucket/ref.yuv
  distorted: s3://bucket/dis.yuv
```

## References

- [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) — operator design
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b
  platform
- [Kubernetes Operator
  pattern](https://kubernetes.io/docs/concepts/extend-kubernetes/operator/)
- [controller-runtime
  v0.25.2](https://pkg.go.dev/sigs.k8s.io/controller-runtime@v0.25.2)
