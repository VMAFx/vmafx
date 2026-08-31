# VMAFx Kubernetes integration tests

The `test/e2e/` suite uses [kind](https://kind.sigs.k8s.io/) and
[kuttl](https://kuttl.dev/) to prove an executable Kubernetes runtime contract:
the Helm chart's default server Deployment and opt-in operator start from the
images built for the exact commit, and the server completes a real CPU VMAF
score through the chart Service.

The suite does not claim that the operator creates scoring Pods or trainer
workloads. Those are not responsibilities implemented by the current
reconcilers. See [ADR-0783](../adr/0783-k8s-e2e-integration-test-harness.md) for
the original harness design and the
[2026-08-31 runtime audit](../research/e2e-k8s-runtime-contract-2026-08-31.md)
for the executable-scope correction.

## Prerequisites

| Tool | Minimum version | Install |
| --- | --- | --- |
| Docker | 24.x | <https://docs.docker.com/engine/install/> |
| kind | 0.23 | `go install sigs.k8s.io/kind@v0.23.0` |
| kubectl | 1.30 | <https://kubernetes.io/docs/tasks/tools/> |
| Helm | 3.14 | <https://helm.sh/docs/intro/install/> |
| kuttl | 0.20 | `go install github.com/kudobuilder/kuttl/cmd/kubectl-kuttl@v0.20.0` |
| curl | 8.x | Distribution package |
| Python | 3.11 | Distribution package |

The CI workflow installs its pinned tool versions automatically.

## Running locally

Run these commands from the repository root:

```bash
export KIND_CLUSTER_NAME=vmafx-e2e
export VMAFX_E2E_KUBECONFIG="${TMPDIR:-/tmp}/vmafx-e2e.kubeconfig"
export KUBECONFIG="${VMAFX_E2E_KUBECONFIG}"

# 1. Build the exact images used by the chart and image contract.
docker build --target operator \
  -t ghcr.io/vmafx/vmafx-operator:e2e-test \
  -f docker/Dockerfile.operator .
docker build --target node-cpu \
  -t ghcr.io/vmafx/vmafx-node:e2e-test \
  -f docker/Dockerfile.node .
docker build --target go-server \
  -t ghcr.io/vmafx/vmafx-server:e2e-test \
  -f Dockerfile.go-server .

# 2. Validate the committed raw clips and generate geometry-bearing Y4M files.
bash test/e2e/fixtures/gen-tiny-yuv.sh

# 3. Create the cluster, install CRDs, and preload locally available images.
bash test/e2e/kind-cluster.sh

# 4. Run the chart and scoring contract.
kubectl kuttl test \
  --config test/e2e/kuttl-tests/kuttl-test.yaml

# 5. Tear down the cluster when finished.
TEARDOWN=1 bash test/e2e/kind-cluster.sh
```

`VMAFX_E2E_KUBECONFIG` is mandatory and must be an absolute path dedicated to
the disposable cluster; `KUBECONFIG` must equal it. Cluster creation can write
an absent dedicated file, but refuses any pre-existing file unless it already
proves the exact local kind identity. Before applying CRDs, running kuttl,
collecting logs, scoring, or deleting the cluster, the harness verifies that
its current context is exactly `kind-${KIND_CLUSTER_NAME}` and that the API
server is a loopback endpoint. It refuses a shared, symlinked, or remote
Kubernetes context, and a failed teardown guard remains a visible failure.

The Helm step pins `image.tag` and `operator.image.tag` to `e2e-test` and sets
both pull policies to `Never`. A missing local image therefore fails rather
than silently testing a registry artifact. `gpu.vendor=cpu` selects the chart's
documented CPU mode while leaving the default `Deployment` workload unchanged.
The main Service additionally selects `app.kubernetes.io/component: server`,
so enabling the operator cannot route scoring traffic to its metrics port.

The raw `ref.yuv` and `dist.yuv` files are committed. The generator validates
their exact SHA-256 and 64x64, eight-frame size, then creates Y4M wrappers
because the REST score request carries paths, not explicit frame dimensions.
The test mounts those wrappers through a test-only ConfigMap at `/fixtures`.
The score helper chooses an unused loopback port unless
`VMAFX_E2E_LOCAL_PORT` is explicitly set, avoiding collisions with unrelated
developer port-forwards.

## Test case

| Directory | What is exercised |
| --- | --- |
| `01-chart-cpu-score/` | CRDs become established; the default `vmafx-server` Deployment and enabled operator become available; `/v1/score` returns finite, matching `score` and `features.vmaf` values for the mounted Y4M pair. |

The score check intentionally validates structure and finiteness rather than a
Netflix golden number. Netflix-authored CPU golden assertions remain in their
dedicated test suite and are not modified by this harness.

Four historical cases were removed after the runtime audit proved their
prerequisites do not exist: the operator does not create VmafxJob worker Pods,
does not own VmafxNode heartbeats, and does not create model-training sidecars;
the MinIO setup also never uploaded its fixtures. Replacing those unreachable
assertions with the chart-backed score is a coverage correction, not a reduced
test promise.

## CI integration

`.github/workflows/e2e-k8s.yml` runs nightly, on manual dispatch, and on pull
requests carrying the `run-e2e-k8s` label. Its image job builds and transfers:

- `ghcr.io/vmafx/vmafx-operator:e2e-test`;
- `ghcr.io/vmafx/vmafx-node:e2e-test` from the explicit `node-cpu` target; and
- `ghcr.io/vmafx/vmafx-server:e2e-test` from the release `go-server` target.

The cheap standard-library contract test also runs in the always-on Rules
workflow, so ordinary pull requests cannot change the image target or remove
the executable scoring case without a gate failure. Kuttl XML, cluster
diagnostics, and server/operator logs are uploaded after failures. Kuttl keeps
the namespace for those diagnostics; the workflow then deletes only the named
kind cluster through the same dedicated kubeconfig.

## Troubleshooting

### Server or operator Pod does not start

```bash
kubectl get pods -n vmafx-e2e-test -o wide
kubectl describe deployment -n vmafx-e2e-test vmafx vmafx-operator
kubectl logs -n vmafx-e2e-test deployment/vmafx --tail=200
kubectl logs -n vmafx-e2e-test deployment/vmafx-operator --tail=200
```

An `ErrImageNeverPull` event means the required `e2e-test` image was not loaded
into the named kind cluster. Rebuild it and run `kind load docker-image` with
the same `KIND_CLUSTER_NAME`.

The node image must contain
`/usr/local/share/vmafx/model/vmaf_v0.6.1.json`; a nested `model/model/` path
means the Docker staging copy no longer matches `VMAFX_MODEL_DIR`.

### Score request fails

```bash
kubectl get configmap -n vmafx-e2e-test vmafx-e2e-fixtures
kubectl get pod -n vmafx-e2e-test -l app.kubernetes.io/name=vmafx
kubectl logs -n vmafx-e2e-test deployment/vmafx --tail=200
bash test/e2e/score-smoke.sh
```

The ConfigMap must contain both `ref.y4m` and `dist.y4m`, and the Deployment
must mount it at `/fixtures`. The score script prints the response failure and
server logs before exiting non-zero.

### CRDs do not become established

```bash
kubectl get crd vmafxjobs.vmafx.dev vmafxnodes.vmafx.dev \
  vmafxmodeltrainings.vmafx.dev
kubectl get events -A --sort-by=.lastTimestamp
```

CRD application is fail-closed. `kind-cluster.sh` no longer hides a failed
Helm workload behind a direct-apply fallback.

## Adding a test case

Add a numbered directory under `test/e2e/kuttl-tests/` and document the
production component that creates every asserted object or status field. New
cases must provision their fixtures, images, and Services explicitly and must
fail when an exact-head prerequisite is absent. Update
`scripts/ci/test_e2e_runtime_contract.py` when the image-transfer or core smoke
contract changes. Use a descriptive filename such as `NN-ready.yaml` for a
command-backed `TestStep`; kuttl reserves `NN-assert.yaml` for declarative
object matching.
