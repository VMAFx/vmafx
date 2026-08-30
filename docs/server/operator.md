<!-- markdownlint-disable MD060 -->
# vmafx-operator — Kubernetes Operator

`vmafx-operator` is a [kubebuilder](https://kubebuilder.io/) v4 /
controller-runtime v0.19+ Kubernetes operator that watches `VmafxJob`,
`VmafxNode`, and `VmafxModelTraining` custom resources and reconciles their
Pod + status sub-resources.

The operator is built per [ADR-0714](../adr/0714-vmafx-operator-skeleton.md)
and runs as a Kubernetes `Deployment` inside the cluster.

## Quick start (in-cluster)

```bash
# Build and load the operator image.
docker build -f docker/Dockerfile.operator -t vmafx-operator:dev .
kind load docker-image vmafx-operator:dev   # or push to your registry

# Deploy with the Helm chart.
helm upgrade --install vmafx deploy/helm/vmafx/ \
    --set operator.enabled=true \
    --set operator.image.tag=dev
```

## Configuration (12-factor env vars)

All settings accept CLI flags and environment variables.  CLI flags take
precedence over env vars.

| Variable | Flag | Default | Description |
|---|---|---|---|
| `VMAFX_OPERATOR_METRICS_ADDR` | `--metrics-bind-address` | `:8081` | Prometheus metrics endpoint bind address. |
| `VMAFX_OPERATOR_PROBE_ADDR` | `--health-probe-bind-address` | `:8082` | Health probe endpoints (`/healthz`, `/readyz`) bind address. |
| `VMAFX_OPERATOR_LEADER_ELECT` | `--leader-elect` | `false` | Set to `true` to enable leader election for high-availability deployments (multiple operator replicas). |
| `VMAFX_OPERATOR_LOG_LEVEL` | `--log-level` | `info` | Log level for the zap logger: `debug`, `info`, `warn`, `error`. |

See the [full environment variable reference](../usage/env-vars.md) for the
complete cross-surface table.

## Health probes

| Path | Port | Purpose |
|---|---|---|
| `/healthz` | `VMAFX_OPERATOR_PROBE_ADDR` | Liveness — returns `200 OK` when the manager is running. |
| `/readyz` | `VMAFX_OPERATOR_PROBE_ADDR` | Readiness — returns `200 OK` when the cache has synced. |

## Prometheus metrics

Standard controller-runtime metrics are exposed at
`VMAFX_OPERATOR_METRICS_ADDR/metrics`.  Add a `ServiceMonitor` resource to
scrape them with Prometheus Operator.

## Leader election

In a multi-replica deployment set `VMAFX_OPERATOR_LEADER_ELECT=true`.  The
operator uses a `Lease` resource named `vmafx-operator.vmafx.dev` in the
operator's namespace for the lock.

## Custom resource definitions

| CRD | Group | Kind |
|---|---|---|
| VmafxJob | `vmafx.dev/v1` | Job submission and lifecycle tracking |
| VmafxNode | `vmafx.dev/v1` | Worker node registration and capability |
| VmafxModelTraining | `vmafx.dev/v1` | Sidecar training run lifecycle |

Install CRDs from the Helm chart (enabled by default) or manually:

```bash
kubectl apply -f deploy/helm/vmafx/crds/
```

## References

- [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) — operator design
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b platform
- [Kubernetes Operator pattern](https://kubernetes.io/docs/concepts/extend-kubernetes/operator/)
- [controller-runtime v0.19](https://pkg.go.dev/sigs.k8s.io/controller-runtime)
