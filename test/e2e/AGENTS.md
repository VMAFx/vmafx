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
server. Never fall back to process-wide default kubeconfig. Kuttl keeps
resources for diagnostics; only `kind-cluster.sh --teardown` deletes exact
named cluster.

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
wrappers so file-path REST API infers 64x64 geometry. Scoring smoke
must not add or modify Netflix golden-score assertions.

Suite coupled to `.github/workflows/e2e-k8s.yml` +
`scripts/ci/test_e2e_runtime_contract.py`: operator, CPU node, Go server
images built/exported/loaded together, even though default chart does not
enable node workload. Update all three surfaces in one change.
