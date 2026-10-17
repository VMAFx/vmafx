# VMAFx Kubernetes integration tests

The `test/e2e/` suite uses [kind](https://kind.sigs.k8s.io/) and
[kuttl](https://kuttl.dev/) to prove an executable Kubernetes runtime contract:
the Helm chart's default server Deployment and opt-in operator start from the
images built for the exact commit, and the server completes a real CPU VMAF
score through the chart Service. A second case runs two `vmafx-controller`
replicas on the PostgreSQL job store, kills one of them and the node of a
running job mid-job, and checks that every job is still completed exactly
once ([ADR-2350](../adr/2350-cloud-native-platform.md)).

The suite does not claim that the operator creates scoring Pods or trainer
workloads. Those are not responsibilities implemented by the current
reconcilers. See [ADR-0783](../adr/0783-k8s-e2e-integration-test-harness.md) for
the original harness design and the
[2026-08-31 runtime audit](../research/e2e-k8s-runtime-contract-2026-08-31.md)
for the executable-scope correction.

## Prerequisites

| Tool | Version CI pins | Install |
| --- | --- | --- |
| Docker | any recent release | <https://docs.docker.com/engine/install/> |
| kind | v0.33.0 | <https://kind.sigs.k8s.io/docs/user/quick-start/#installation> |
| kubectl | v1.37.0 | <https://kubernetes.io/docs/tasks/tools/> |
| Helm | v4.2.4 | <https://helm.sh/docs/intro/install/> |
| kuttl | v0.26.0 | <https://kuttl.dev/docs/cli.html> |
| curl, python3 | distribution package | used by `test/e2e/score-smoke.sh`, `test/e2e/fixtures/gen-tiny-yuv.sh` and `test/e2e/controller-ha/` |

The pins live in the `env` block of `.github/workflows/e2e-k8s.yml`. Earlier
versions may work locally; CI is the reference.

## Running locally

Run the cluster in a virtual machine, not on your workstation's Docker. kind
runs the kubelet in a privileged container, and the kubelet writes kernel
settings that containers do not isolate (`vm.overcommit_memory`,
`kernel.panic_on_oops`, `kernel.panic`). Run directly, a kind cluster sets
them on the workstation's own kernel: with memory overcommit enabled, a test
elsewhere on the machine that deliberately requests far more memory than
exists succeeds and fills RAM and swap. In a VM the cluster changes only the
VM's kernel.

`test/e2e/incus-kind-vm.sh` creates that VM with [incus](https://linuxcontainers.org/incus/):
a Debian 13 VM with its own profile (named after the VM, never the shared
`default` profile; 6 CPUs, 12 GiB, a 20 GiB root disk on the `default` pool, a
NIC on `incusbr0`, all overridable through the variables in the script's
header), Docker from Debian and the kind release the workflow pins (checked
against its published SHA-256). With `VMAFX_E2E_INCUS_VM` set,
`kind-cluster.sh` runs kind inside the VM, publishes the API server on the
VM's address, copies the kubeconfig out, and copies your local images in; the
other commands run unchanged on the host. You need membership in
`incus-admin` and KVM.

Run these commands from the repository root:

```bash
export KIND_CLUSTER_NAME=vmafx-e2e
export VMAFX_E2E_INCUS_VM=vmafx-e2e-kind
export VMAFX_E2E_KUBECONFIG="${TMPDIR:-/tmp}/vmafx-e2e.kubeconfig"
export KUBECONFIG="${VMAFX_E2E_KUBECONFIG}"

# 0. Create the VM (once; later runs reuse it).
bash test/e2e/incus-kind-vm.sh create

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
docker build --target controller --build-arg VMAF_BUILD_JOBS=4 \
  -t ghcr.io/vmafx/vmafx-controller:e2e-test \
  -f docker/Dockerfile.controller .
docker build --target driver \
  -t ghcr.io/vmafx/vmafx-e2e-driver:e2e-test \
  -f docker/Dockerfile.e2e-driver .

# 2. Validate the committed raw clips and generate geometry-bearing Y4M files.
bash test/e2e/fixtures/gen-tiny-yuv.sh

# 3. Create the cluster in the VM, install CRDs, and preload locally
#    available images.
bash test/e2e/kind-cluster.sh

# 4. Run every case, or one with --test 02-controller-ha.
kubectl kuttl test \
  --config test/e2e/kuttl-tests/kuttl-test.yaml

# 5. Tear down the cluster, then the VM and its profile.
TEARDOWN=1 bash test/e2e/kind-cluster.sh
bash test/e2e/incus-kind-vm.sh destroy
```

On a disposable machine (the CI runner) leave `VMAFX_E2E_INCUS_VM` unset and
skip steps 0 and the VM teardown: `kind-cluster.sh` then uses the local Docker.

`VMAFX_E2E_KUBECONFIG` is mandatory and must be an absolute path dedicated to
the disposable cluster; `KUBECONFIG` must equal it. Cluster creation can write
an absent dedicated file, but refuses any pre-existing file unless it already
proves the exact local kind identity. Before applying CRDs, running kuttl,
collecting logs, scoring, or deleting the cluster, the harness verifies that
its current context is exactly `kind-${KIND_CLUSTER_NAME}` and that the API
server is a loopback endpoint, or, with `VMAFX_E2E_INCUS_VM`, port 6443 at the
address the named VM reports. It refuses a shared, symlinked, or remote
Kubernetes context, and a failed teardown guard remains a visible failure.

The Helm step pins `image.tag` and `operator.image.tag` to `e2e-test` and sets
both pull policies to `Never`. A missing local image therefore fails rather
than silently testing a registry artifact. `gpu.vendor=cpu` selects the chart's
documented CPU mode while leaving the default `Deployment` workload unchanged.
The main Service additionally selects `app.kubernetes.io/component: server`,
so enabling the operator cannot route scoring traffic to its metrics port.

The raw `ref.yuv` and `dist.yuv` files are committed. The generator validates
their exact SHA-256 and 216x160, eight-frame size, then creates Y4M wrappers
because the REST score request carries paths, not explicit frame dimensions.
The test mounts those wrappers through a test-only ConfigMap at `/fixtures`,
created with `kubectl apply --server-side`: client-side apply would copy the
base64 payload into the `last-applied-configuration` annotation, which the API
server caps at 256 KiB.

216x160 is the smallest 4:2:0 size the default model accepts. The request names
no model, so the server scores with `vmaf_v1.0.16_3d0h`
([ADR-1169](../adr/1169-default-model-v1-0-16.md)). Its `cambi` feature needs
one side of at least 216 pixels and its `speed_chroma` feature needs both
chroma planes at least 80 pixels, so the luma plane needs at least 160 on both
sides. On a smaller clip the CLI refuses the input and `/v1/score` answers
HTTP 500. `scripts/ci/test_e2e_runtime_contract.py` checks the generator's
`WIDTH` and `HEIGHT` against the thresholds in `core/src/feature/` on every
pull request, and checks that the Y4M pair fits the 1 MiB ConfigMap limit.
The score helper chooses an unused loopback port unless
`VMAFX_E2E_LOCAL_PORT` is explicitly set, avoiding collisions with unrelated
developer port-forwards.

## Test cases

| Directory | What is exercised |
| --- | --- |
| `01-chart-cpu-score/` | CRDs become established; the default `vmafx-server` Deployment and enabled operator become available; `/v1/score` returns finite, matching `score` and `features.vmaf` values for the mounted Y4M pair. |
| `02-controller-ha/` | The CloudNativePG operator installs from its pinned manifest; the chart's database `Cluster` becomes ready, the migration Job completes, two controller replicas and a CPU node become available; a controller replica and the node of a running job are killed mid-job, and every job completes with a finite score, each by exactly one attempt. |

### Controller failover case

`02-controller-ha` installs the chart as release `vmafx` in namespace
`vmafx-ha` with `test/e2e/controller-ha/values.yaml`: `controller.replicas: 2`,
`controller.store.backend: postgres` on a one-instance CloudNativePG `Cluster`,
short leases (30 s) and sweeps (2 s), one CPU node, and requests and limits
sized for a kind node. `install-cnpg.sh` downloads the CloudNativePG 1.30.1
release manifest, checks its SHA-256, and runs the operator from the image
digest rather than the manifest's tag.

`run-driver.sh` then runs the driver (`test/e2e/controller-ha/driver/`, image
`docker/Dockerfile.e2e-driver`) as a Job in the same namespace:

1. The driver serves a synthetic 216x160 Y4M pair over HTTP; its Service is the
   case's scoring root (`auth.scoringRoots`), so the nodes stream the inputs
   from it.
2. It submits four jobs and scales the node pool to two.
3. Every stream sends its header and first frame and then waits, so each
   first attempt is still running when the driver kills the node pod of a
   running job and one controller pod (grace period 0).
4. It releases every stream except those of the killed pod's address: a pod
   deleted with grace period 0 leaves the API before the kubelet has stopped
   its container, and a released stream could still complete there. The
   other node finishes its jobs, and the killed node's job returns to the
   queue when its lease expires and runs again on another node.
5. It waits until every job completed with a finite score and reads
   `job_attempts` in the database: each job has exactly one completed
   attempt, and the killed job has at least two attempts.

The driver prints one JSON summary line (jobs, killed pods, attempts and
outcomes per job); `run-driver.sh` passes only when its `ok` field is true.

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
requests carrying the `run-e2e-k8s` label, which
`.github/workflows/pr-label-lanes.yml` calls it for. Its image job builds and transfers:

- `ghcr.io/vmafx/vmafx-operator:e2e-test`;
- `ghcr.io/vmafx/vmafx-node:e2e-test` from the explicit `node-cpu` target;
- `ghcr.io/vmafx/vmafx-server:e2e-test` from the release `go-server` target;
- `ghcr.io/vmafx/vmafx-controller:e2e-test` from the release `controller`
  target; and
- `ghcr.io/vmafx/vmafx-e2e-driver:e2e-test`, the failover driver, a test image
  that is never published.

The cheap standard-library contract test also runs in the always-on Rules
workflow, so ordinary pull requests cannot change the image target or remove
the executable scoring case without a gate failure. Kuttl XML, cluster
diagnostics, and server/operator logs are uploaded after failures. Kuttl keeps
the namespace for those diagnostics; the workflow then deletes only the named
kind cluster through the same dedicated kubeconfig.

Every run creates a new cluster, so the harness installs the chart and never
upgrades an existing release. The one-time upgrade from a v1.0.0-rc.1 release
([upgrade guide](../development/k8s-deployment.md#upgrading-from-100-rc1)) was
checked by hand on kind; ADR-1353 records the steps and results.

## Troubleshooting

### Server or operator Pod does not start

```bash
kubectl get pods -n vmafx-e2e-test -o wide
kubectl describe deployment -n vmafx-e2e-test vmafx vmafx-operator
kubectl logs -n vmafx-e2e-test --tail=200 \
  -l app.kubernetes.io/instance=vmafx,app.kubernetes.io/component=server
kubectl logs -n vmafx-e2e-test deployment/vmafx-operator --tail=200
```

Selecting the server Pods by label prints the log of every server Pod;
`kubectl logs deployment/vmafx` prints one. Before 1.0.0-rc.2 the server
Deployment's selector matched every Pod of the release, the operator's
included, so `deployment/vmafx` could print the operator's log
([ADR-1353](../adr/1353-helm-server-component-selector.md)).

An `ErrImageNeverPull` event means the required `e2e-test` image was not loaded
into the named kind cluster. Rebuild it and run `kind load docker-image` with
the same `KIND_CLUSTER_NAME`.

The node image must contain
`/usr/local/share/vmafx/model/vmaf_v0.6.1.json`; a nested `model/model/` path
means the Docker staging copy no longer matches `VMAFX_MODEL_DIR`.

### Score request fails

```bash
kubectl get configmap -n vmafx-e2e-test vmafx-e2e-fixtures
kubectl get pod -n vmafx-e2e-test -l app.kubernetes.io/component=server
kubectl logs -n vmafx-e2e-test --tail=200 \
  -l app.kubernetes.io/instance=vmafx,app.kubernetes.io/component=server
bash test/e2e/score-smoke.sh
```

The ConfigMap must contain both `ref.y4m` and `dist.y4m`, and the Deployment
must mount it at `/fixtures`. On failure the score script prints the
`/v1/score` response body and the logs of the Pods behind the Service. A body
such as `requires feature 'cambi', which needs width or height >= 216` means
the fixtures are smaller than the default model accepts; see the fixture
section above.

### `helm upgrade` fails with `spec.selector` ... `field is immutable`

The reused cluster still holds a `vmafx` release installed from a chart older
than 1.0.0-rc.2, whose server Deployment selector cannot be changed in place.
Tear the cluster down with `TEARDOWN=1 bash test/e2e/kind-cluster.sh`, or
delete the server Deployment as the
[upgrade guide](../development/k8s-deployment.md#upgrading-from-100-rc1)
describes, and run kuttl again.

### CRDs do not become established

```bash
kubectl get crd vmafxjobs.vmafx.dev vmafxnodes.vmafx.dev \
  vmafxmodeltrainings.vmafx.dev
kubectl get events -A --sort-by=.lastTimestamp
```

CRD application is fail-closed. `kind-cluster.sh` no longer hides a failed
Helm workload behind a direct-apply fallback.

### Controller failover case fails

```bash
kubectl get clusters.postgresql.cnpg.io,jobs,pods -n vmafx-ha -o wide
kubectl logs -n vmafx-ha -l app.kubernetes.io/component=migrate --tail=100
kubectl logs -n vmafx-ha -l app.kubernetes.io/component=controller --prefix --tail=200
kubectl logs -n vmafx-ha job/vmafx-e2e-driver
```

A controller that stays not ready usually waits for the migration Job, which
waits for the database. The driver's summary names the killed job and the
attempts of every job: a job with no completed attempt, or two, is the
failure the case exists to catch.

## Adding a test case

Add a numbered directory under `test/e2e/kuttl-tests/` and document the
production component that creates every asserted object or status field. New
cases must provision their fixtures, images, and Services explicitly and must
fail when an exact-head prerequisite is absent. Update
`scripts/ci/test_e2e_runtime_contract.py` when the image-transfer or core smoke
contract changes. Use a descriptive filename such as `NN-ready.yaml` for a
command-backed `TestStep`; kuttl reserves `NN-assert.yaml` for declarative
object matching.
