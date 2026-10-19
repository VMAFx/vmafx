# Kubernetes E2E invariants

Suite must assert executable production behavior. Stable boundary: Helm
chart's default `Deployment` workload + opt-in operator, exact `e2e-test`
images loaded into kind, `imagePullPolicy: Never`. CPU case must complete
real `/v1/score` through chart Service, validate finite response values.
Readiness alone is not an end-to-end score.

CPU bootstrap: no cert-manager or device-plugin dependency. Webhooks
disabled. `gpu.vendor=cpu` requests no GPU resource. Do not restore
unrelated privileged or remote components as prerequisites for this lane.

Every Kubernetes read, mutation, cleanup: use absolute path in
`VMAFX_E2E_KUBECONFIG`. Before continuing, `assert-kind-context.sh`
requires current context `kind-${KIND_CLUSTER_NAME}` + loopback API
server (VM mode: `https://<VM addr>:6443` only). Never fall back to
process-wide default kubeconfig. Kuttl keeps resources for diagnostics;
only `kind-cluster.sh --teardown` deletes exact named cluster.

Readiness commands live in `01-ready.yaml`. Do not rename a
command-backed step to `*-assert.yaml`: kuttl reserves that suffix for
declarative object matching, will wait for a `TestStep` custom resource
that never exists. Fixture volume patch must stay strategic-merge /
idempotent so interrupted local runs retry safely. `score-smoke.sh` binds
an available IPv4 loopback port, must keep exercising chart Service, not
direct Pod or Deployment port-forward.

Do not assert: `VmafxJobReconciler` creates worker Pods; operator writes
`VmafxNode.status.lastHeartbeat`; `VmafxModelTrainingReconciler` creates
trainer Pods or Services. Not implemented as ownership contracts yet. Add
such cases only after production component + every fixture/service
prerequisite exist.

`fixtures/gen-tiny-yuv.sh` validates committed raw files, generates Y4M
wrappers so file-path REST API infers 216x160 geometry. Geometry must
satisfy default model (`vmaf_v1.0.16_3d0h`, ADR-1169): cambi needs one
axis >= 216, speed_chroma needs 4:2:0 luma >= 160 both axes. Smaller ->
CLI refuses input, `/v1/score` 500 (nightly red 2026-09-24..27).
`WIDTH`/`HEIGHT`/`FRAMES` in the script = single source; contract test
checks them against `core/src/feature/` thresholds + ConfigMap 1 MiB cap.
Fixture ConfigMap: `kubectl apply --server-side` only; client-side apply
copies base64 payload into 256 KiB-capped annotation. Server logs: select
by Service selector (`component=server`); `deployment/vmafx` selector
also matches operator Pod. Scoring smoke must not add or modify Netflix
golden-score assertions.

Suite coupled to `.github/workflows/e2e-k8s.yml` +
`scripts/ci/test_e2e_runtime_contract.py`: operator, CPU node, Go server,
controller, failover driver images built/exported/loaded together, even
though default chart leaves node workload off. Update all three
surfaces in one change.

Case `02-controller-ha` (ADR-2350): release `vmafx`, namespace `vmafx-ha`,
values `controller-ha/values.yaml` (2 controller replicas, postgres store on
1-instance CloudNativePG `Cluster`, server replicas 0). CNPG operator from
`install-cnpg.sh` only: manifest SHA-256 checked, operator image rewritten
tag -> digest (2 refs, count checked). Driver Job (`driver.yaml`,
`docker/Dockerfile.e2e-driver`, test image, never pushed) serves inputs:
driver Service URL = `auth.scoringRoots` entry = driver `-fixtures-url`
default; contract test pins all three. Streams hold after first frame until
kill -> killed job provably mid-run; no timing on job length, no CPU-limit
sizing. Kill = node pod of RUNNING job + 1 controller pod, grace 0. Release
all streams except killed pod IP's (grace-0 delete removes pod object
before kubelet stops container; released stream there could still
complete). Pass
= every job completed, finite score, exactly 1 completed attempt in
`job_attempts` (read in maintenance mode), killed job >= 2 attempts.
Driver clip geometry mirrors `fixtures/gen-tiny-yuv.sh` (contract test).
`backoffLimit: 0`: rerun = fresh scenario via `run-driver.sh`.
Local runs: kind only inside incus VM (`incus-kind-vm.sh`, own profile, own
kernel; `VMAFX_E2E_INCUS_VM`), never kind/k3d/kubelet in host container:
kubelet writes non-namespaced host sysctls (`vm.overcommit_memory=1`,
`kernel.panic_on_oops=1`). Guard then accepts only `https://<VM addr>:6443`.
CI hosted runner: plain kind, VM unset.
