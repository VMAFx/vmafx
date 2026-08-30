# VMAFx Kubernetes Integration Tests

The `test/e2e/` directory contains a [kuttl](https://kuttl.dev/) test suite that
exercises the VMAFx Phase 4b platform (operator, CRDs, vmafx-node, rclone
streaming, and the online sidecar trainer) against a real [kind](https://kind.sigs.k8s.io/)
cluster. See [ADR-0783](../adr/0783-k8s-e2e-integration-test-harness.md) for the
design rationale.

## Prerequisites

| Tool | Minimum version | Install |
| ------ | ---------------- | --------- |
| Docker | 24.x | <https://docs.docker.com/engine/install/> |
| kind | 0.23 | `go install sigs.k8s.io/kind@v0.23.0` |
| kubectl | 1.30 | <https://kubernetes.io/docs/tasks/tools/> |
| helm | 3.14 | <https://helm.sh/docs/intro/install/> |
| kuttl | 0.20 | `go install github.com/kudobuilder/kuttl/cmd/kubectl-kuttl@v0.20.0` |

All tools are installed automatically when running via the CI workflow
(`.github/workflows/e2e-k8s.yml`).

## Running locally

```bash
# 1. Build the e2e images (operator + node, cpu variant).
docker build -t ghcr.io/vmafx/vmafx-operator:e2e-test -f Dockerfile.operator .
docker build -t ghcr.io/vmafx/vmafx-node:e2e-test     -f Dockerfile.node --build-arg BACKEND=cpu .

# 2. Generate YUV fixtures (idempotent — skips if already present).
bash test/e2e/fixtures/gen-tiny-yuv.sh

# 3. Spin up the kind cluster (installs cert-manager, CRDs, device plugin).
bash test/e2e/kind-cluster.sh

# 4. Pre-load images into the kind cluster (avoids DockerHub pull limits).
kind load docker-image ghcr.io/vmafx/vmafx-operator:e2e-test --name vmafx-e2e
kind load docker-image ghcr.io/vmafx/vmafx-node:e2e-test     --name vmafx-e2e

# 5. Run the kuttl test suite.
kubectl kuttl test test/e2e/kuttl-tests/ \
    --namespace vmafx-e2e-test \
    --timeout 300

# 6. (Optional) Tear down the cluster when done.
TEARDOWN=1 bash test/e2e/kind-cluster.sh
```

## Test cases

| # | Directory | What is exercised |
| --- | ----------- | ------------------- |
| 01 | `01-operator-installs/` | All three CRDs (`VmafxJob`, `VmafxNode`, `VmafxModelTraining`) reach `Established`. Operator `Deployment` reaches `Available`. |
| 02 | `02-vmafxjob-creates-pod/` | Submitting a `VmafxJob` causes the operator to create a worker `Pod`. The job transitions `Pending → Running → Succeeded` and `status.score` is populated. |
| 03 | `03-node-heartbeat/` | A `VmafxNode` CR backed by a stub `/healthz` server receives a `lastHeartbeat` timestamp within 30 s of creation, confirming the operator's health-probe reconciler is wired. |
| 04 | `04-rclone-score/` | Ref + dist YUVs are fetched via `rclone://` from an in-cluster MinIO stand-in. The job scores them with the CPU backend and returns a result. |
| 05 | `05-sidecar-trainer/` | A `VmafxModelTraining` CR ingests one feedback sample from a completed `VmafxJob` and pushes a checkpoint to an in-cluster OCI registry stub. |

Tests run serially (`parallel: 1` in `kuttl-test.yaml`) because later cases
depend on cluster state created by earlier ones.

## GPU strategy

On hosts with an NVIDIA GPU and `nvidia-container-runtime`, `kind-cluster.sh`
installs the [NVIDIA device plugin](https://github.com/NVIDIA/k8s-device-plugin)
so real `nvidia.com/gpu` resource requests are schedulable.

On GPU-less hosts (GitHub-hosted runners, developer laptops without NVIDIA),
the script installs
[squat/k8s-fakedeviceplugin](https://github.com/squat/k8s-fakedeviceplugin)
which advertises two synthetic `nvidia.com/gpu` slots. The vmafx-node image
activates the CPU fallback path automatically when no real GPU is present, so
all five test cases pass on CPU-only hardware.

Force the fake-GPU path even on an NVIDIA host:

```bash
GPU_MODE=fake bash test/e2e/kind-cluster.sh
```

## CI integration

The workflow (`.github/workflows/e2e-k8s.yml`) runs:

- **Nightly** at 03:47 UTC unconditionally.
- **On PRs** only when the `run-e2e-k8s` label is applied. This keeps normal
  PR CI fast; the label can be added by maintainers when a PR touches operator,
  node, or CRD code.
- **On demand** via `workflow_dispatch` with optional `cluster_name` and
  `keep_cluster` inputs (the latter is useful for post-failure debugging).

Test results are published as XML via `publish-unit-test-result-action` and
attached as workflow artifacts under `kuttl-e2e-results`.

## Troubleshooting

### Operator Pod not starting

```bash
kubectl describe pod -n vmafx-system -l control-plane=controller-manager
kubectl logs -n vmafx-system deployment/vmafx-operator
```

### CRDs not established after 120 s

Check that cert-manager webhooks are healthy:

```bash
kubectl get pods -n cert-manager
kubectl describe crd vmafxjobs.vmafx.dev
```

### VmafxJob stuck in Pending

```bash
kubectl describe vmafxjob <name> -n vmafx-e2e-test
kubectl get events -n vmafx-e2e-test --sort-by=.lastTimestamp
```

### Fake-device-plugin not advertising GPUs

```bash
kubectl describe node | grep -A5 nvidia.com
kubectl logs -n kube-system daemonset/fake-gpu-device-plugin
```

## Adding a new test case

1. Create a new directory `test/e2e/kuttl-tests/NN-my-test/`.
2. Add numbered YAML steps following the kuttl
   [TestStep](https://kuttl.dev/docs/testing/reference.html#teststep) schema.
3. Add a row to the test-cases table above.
4. If the test introduces new resource dependencies (images, external services),
   update `kind-cluster.sh` to install them during cluster bootstrap.
