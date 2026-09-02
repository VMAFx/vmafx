<!-- markdownlint-disable MD013 -->
# Kubernetes E2E runtime-contract audit — 2026-08-31

**Date:** 2026-08-31
**Base:** `16b9f99fa5742491d545854f7430de348e002755`
**Failed run:** `33380856029`, job `99452688028`

## Finding

The scheduled Kubernetes E2E run failed before cluster creation while building
the image labelled “cpu variant.” The workflow invoked
`docker/Dockerfile.node` without a target and supplied `BACKEND=cpu` plus
`GIT_SHA`. That Dockerfile declares or consumes neither argument and has no
Git-SHA label; Docker build arguments do not select multi-stage targets.

`docker buildx build --call=targets -f docker/Dockerfile.node .` identifies
`node-sycl` as the implicit default because it is the last stage. The failed
log confirms that path: after finishing the common FFmpeg builder, BuildKit
entered `node-sycl` and failed at line 372 while looking for
`/opt/intel/oneapi/compiler/latest/lib/libsycl.so*` in the pinned Intel image.
Selecting `target: node-cpu` is therefore required for the nominal CPU lane.

An execution audit found that repairing only that target would expose several
later failures rather than make the suite meaningful:

- The chart's default workload is the `vmafx-server` Deployment, but the
  workflow built, exported, and loaded only the operator and node images.
- Enabling the operator made the main HTTP Service select both server and
  operator Pods because its selector contained only the shared release labels.
  Port-forwarding the Service could therefore choose the operator's metrics
  port 8080 and return HTTP 404 for every server route.
- `docker/Dockerfile.node` copied `model/` into `/dist/model/`, producing
  `/usr/local/share/vmafx/model/model/…` while `VMAFX_MODEL_DIR` names
  `/usr/local/share/vmafx/model`. The CPU image built successfully but its
  packaged model could not be resolved at the configured root.
- `test/e2e/kind-cluster.sh` used a full Helm release as a CRD installer before
  the runtime images were loaded. Its `|| kubectl apply` fallback converted a
  failed server rollout into apparent CRD-bootstrap success.
- `VmafxJobReconciler` is a remote-controller status bridge. It does not create
  worker Pods, so both the local-Pod and rclone scoring cases waited for an
  object that no production code could create. The MinIO setup also created a
  bucket without uploading either fixture.
- `VmafxNode.status.lastHeartbeat` is node-agent-owned (ADR-1069). The old case
  supplied an ignored annotation and incorrectly expected the operator to
  write that sentinel through an `echoserver` Pod.
- `VmafxModelTrainingReconciler` polls an already-running trainer sidecar. The
  old case created neither its trainer Pod nor Service, so its asserted
  checkpoint path was structurally unreachable.
- The first real Y4M request reached the Go scorer but returned HTTP 500:
  `Scorer.Score` passed `-m /absolute/model.json`, while the current CLI model
  grammar requires `-m path=/absolute/model.json`. Existing unit stubs ignored
  the model option, so they could not detect that every file-backed server
  score failed before libvmaf ran.

These cases were false specifications, not working coverage. Keeping them
would make every scheduled run fail for behavior the repository intentionally
does not implement.

## Executable replacement

The repaired lane retains the explicit CPU node image build and adds the exact
production Go server target used by releases:
`Dockerfile.go-server --target go-server`. All three tagged images (operator,
node, server) must be exported and explicitly loaded into kind. The Helm test
sets both pull policies to `Never`, so a missing exact-head image fails closed
instead of falling back to a registry tag.

The CPU bootstrap no longer installs cert-manager or a privileged fake GPU
device plugin. Webhooks are disabled in the tested operator configuration and
`gpu.vendor=cpu` requests no device resource, so those external components
were unrelated failure points rather than test prerequisites. CRDs are applied
directly and must become Established before the single Helm release starts.

During local validation, a separate process changed the machine's shared
default kube context between retries. Kuttl created a previously absent test
namespace on that non-kind context, and the next failed retry added only the
fixture ConfigMap. A read-only inventory showed no workload resources, custom
resources, or Secrets. Kuttl issued its normal namespace deletion request; the
namespace reported `Terminating` with only the Kubernetes namespace finalizer.
Validation stopped and no force-finalization or further mutation was
performed. This evidence is why the repaired harness requires a dedicated
kubeconfig, exact `kind-<cluster>` current context, and loopback API server
before applies, diagnostics, scoring, or exact-name teardown.

One kuttl case now installs the chart's default Deployment workload, opts the
test runner into the CPU vendor, enables the operator, and waits for both real
Deployments. The committed 64x64 raw fixtures are size- and SHA-256-validated,
then wrapped as Y4M so the file-path REST API can infer geometry. A test-only
ConfigMap is mounted into the server Pod, then `score-smoke.sh` calls
`/v1/score` through the chart Service and requires finite, internally
consistent `score` and `features.vmaf` values. It deliberately does not assert
a Netflix golden score. Kuttl preserves the namespace for failure diagnostics
inside the disposable cluster; cleanup is the explicit kind-cluster deletion
guarded by the same isolated context check.

The server Pod carries `app.kubernetes.io/component: server`, and every main
Service/PDB/monitoring consumer adds that discriminator. The Deployment and
StatefulSet immutable selectors remain unchanged so existing Helm releases can
upgrade safely. The fixture mount uses a strategic merge patch so a retry is
idempotent; the readiness commands live in `01-ready.yaml` because kuttl treats
the reserved `*-assert.yaml` suffix as a declarative object assertion. The
score port is selected dynamically on loopback to avoid unrelated local
port-forwards.

This replacement increases truthful end-to-end coverage: it exercises the
chart, exact built server image, Service routing, HTTP handler, Go scorer,
packaged model, `vmaf` subprocess, libvmaf, and JSON response. The removed
scenarios exercised none of those paths because their prerequisites could
never exist.

The scorer adapter now emits the CLI's `path=` model parameter and a focused
argv regression records the spawned command. This is deliberately narrower
than adding raw-YUV dimensions to the REST schema: Y4M already provides the
geometry needed by the documented file-path endpoint.

The node image copies `model/.` into the staging directory and build-fails
unless `vmaf_v0.6.1.json` is at its root. Final-image inspection confirmed the
same file at `/usr/local/share/vmafx/model/vmaf_v0.6.1.json`, aligned with
`VMAFX_MODEL_DIR`.

The standard-library contract test checks the image targets, image transfer,
CPU chart settings, exact-local-image policy, replacement smoke, and absence
of the four impossible cases. It runs in the always-on Rules workflow for
ordinary PRs and again before the label/schedule-gated image build.

The same audit window exposed an independent master-CI collision. Scheduled
Security Scans run `33389099701` and master-push run `33388320315` both used
`security-${{ github.workflow }}-refs/heads/master`, so the schedule canceled
the push's in-progress C/C++ CodeQL analysis. The concurrency key now includes
`github.event_name`: newer pushes can still supersede older pushes, and newer
schedules can supersede older schedules, but the two coverage classes cannot
cancel each other. A separate dependency-free contract runs in the always-on
Rules gate because a workflow cannot test its own concurrency key after GitHub
has already applied it.

## Alternatives considered

| Alternative | Decision | Reason |
| --- | --- | --- |
| Explicit `node-cpu` and `go-server` targets plus a real chart score | Accept | It tests release artifacts and supported runtime behavior directly. |
| Keep `BACKEND=cpu` | Reject | The argument is undeclared and cannot select a Docker stage. |
| Reorder `Dockerfile.node` | Reject | It makes correctness depend on file order and changes unrelated GPU layout. |
| Keep the four legacy kuttl cases | Reject | Each requires a workload or status writer absent by design; they are false specifications. |
| Implement the missing distributed workers/trainers in this CI repair | Reject | That is new product architecture, not a repair of the existing scheduled test. |
| Add raw-YUV dimensions to the REST API | Reject | The supported Y4M format is self-describing; changing the public request schema is unnecessary scope. |
| Replace kuttl with a health-only probe | Reject | Readiness alone does not prove that the image can perform real libvmaf work. |
| Port-forward the server Deployment directly | Reject | It would hide a broken chart Service selector and omit Service routing from the contract. |
| Pull `e2e-test` images from GHCR | Reject | A stale registry tag would not prove the pull-request head under test. |

## Verification

```bash
python3 scripts/ci/test_e2e_runtime_contract.py
bash test/e2e/fixtures/gen-tiny-yuv.sh
docker buildx build --call=outline --target node-cpu \
  -f docker/Dockerfile.node .
docker build --target node-cpu -t vmafx-node:e2e-model-layout \
  -f docker/Dockerfile.node .
docker buildx build --call=outline --target go-server \
  -f Dockerfile.go-server .
actionlint .github/workflows/e2e-k8s.yml
actionlint .github/workflows/rule-enforcement.yml
export KIND_CLUSTER_NAME=vmafx-e2e-contract
export VMAFX_E2E_KUBECONFIG="${TMPDIR:-/tmp}/vmafx-e2e-contract.kubeconfig"
export KUBECONFIG="${VMAFX_E2E_KUBECONFIG}"
bash test/e2e/kind-cluster.sh
bash test/e2e/assert-kind-context.sh
kubectl kuttl test --config test/e2e/kuttl-tests/kuttl-test.yaml
TEARDOWN=1 bash test/e2e/kind-cluster.sh --teardown
```

The isolated local run completed the single kuttl case in 13.67 seconds. The
Service selector contained the release labels plus `component=server`, its
only selected Pod used `vmafx-server:e2e-test`, and the real score returned
finite matching values (`97.428043`). That value is execution evidence, not a
new golden assertion. Exact-name kind teardown succeeded and a subsequent kind
inventory contained no cluster with that name.

No ADR is required: the accepted behavior is fixed by the existing release
image, Helm workload, reconciler ownership contracts, and the direct request
to replace structurally impossible scenarios. This is a deterministic CI bug
fix rather than a new architecture or user-facing policy.
