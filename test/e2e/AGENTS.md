# Kubernetes E2E invariants

The suite must assert executable production behavior. Its stable boundary is
the Helm chart's default `Deployment` workload plus the opt-in operator, using
the exact `e2e-test` images loaded into kind and `imagePullPolicy: Never`. The
CPU case must complete a real `/v1/score` through the chart Service and validate
finite response values; readiness alone is not an end-to-end score.

The CPU bootstrap has no cert-manager or device-plugin dependency. Webhooks
are disabled and `gpu.vendor=cpu` requests no GPU resource; do not restore
unrelated privileged or remote components as prerequisites for this lane.

Every Kubernetes read, mutation, and cleanup must use the absolute path in
`VMAFX_E2E_KUBECONFIG`. Before continuing, `assert-kind-context.sh` requires
current context `kind-${KIND_CLUSTER_NAME}` and a loopback API server. Never
fall back to the process-wide default kubeconfig. Kuttl keeps resources for
diagnostics; only `kind-cluster.sh --teardown` deletes the exact named cluster.

The readiness commands live in `01-ready.yaml`. Do not rename a command-backed
step to `*-assert.yaml`: kuttl reserves that suffix for declarative object
matching and will wait for a `TestStep` custom resource that never exists. The
fixture volume patch must remain strategic-merge/idempotent so interrupted
local runs can be retried safely. `score-smoke.sh` binds an available IPv4
loopback port and must continue to exercise the chart Service, not a direct Pod
or Deployment port-forward.

Do not assert that `VmafxJobReconciler` creates worker Pods, that the operator
writes `VmafxNode.status.lastHeartbeat`, or that
`VmafxModelTrainingReconciler` creates trainer Pods or Services. Those are not
implemented ownership contracts. Add such cases only after the production
component and every fixture/service prerequisite exist.

`fixtures/gen-tiny-yuv.sh` validates the committed raw files and generates Y4M
wrappers so the file-path REST API can infer 64x64 geometry. The scoring smoke
must not add or modify Netflix golden-score assertions.

The suite is coupled to `.github/workflows/e2e-k8s.yml` and
`scripts/ci/test_e2e_runtime_contract.py`: operator, CPU node, and Go server
images are built/exported/loaded together, even though the default chart does
not enable the node workload. Update all three surfaces in one change.
