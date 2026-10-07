---
paths:
  - .github/workflows/build.yml
  - .github/workflows/helm-chart.yml
  - .github/workflows/e2e-k8s.yml
invariant: ORT/Helm version+SHA together; E2E targets; kuttl outcome checked; downloads bounded; diagnostics not silenced.
---
# ONNX Runtime and Helm versions, digests, and E2E contract

## ONNX Runtime release version and digest stay coupled

Linux all-backends row in [`workflows/build.yml`](../workflows/build.yml)
downloads ONNX Runtime into `RUNNER_TEMP`, verifies release asset's SHA-256,
only then extracts it with `sudo`. Keep `ORT_VERSION` and `ORT_SHA256`
updated together from official GitHub release asset metadata. Never pipe
retrying `curl` transfer directly into `tar`: retries require file-backed
output, and privileged extraction must not see unverified bytes.

## Helm workflow version and digest stay coupled

[`workflows/helm-chart.yml`](../workflows/helm-chart.yml) and
[`workflows/e2e-k8s.yml`](../workflows/e2e-k8s.yml) install same pinned Helm
archive from `get.helm.sh`. Keep `HELM_VERSION`, `HELM_SHA256`, and verified
file-backed extraction sequence identical in both workflows. Never restore
moving `helm/helm@main` installer or pipe network bytes into shell; update
digest from Helm's official checksum whenever version changes.

E2E workflow deliberately gives kuttl step `continue-on-error` so
diagnostics and XML can still upload. Its final assertion must inspect
`steps.kuttl.outcome` (not `conclusion`), fail unless it is `success`;
otherwise command failures are converted into green workflow.

E2E image build must explicitly select `target: node-cpu` for
`docker/Dockerfile.node` and `target: go-server` for `Dockerfile.go-server`.
`BACKEND=cpu` is not declared node-Dockerfile argument, cannot select
multi-stage target; without `target`, BuildKit chooses last stage
(`node-sycl`). Export and load operator, node, and server `e2e-test` tags as
one contract. Node image model copy must remain flat at configured
`VMAFX_MODEL_DIR`. Chart smoke sets both pull policies to `Never`, installs
default server Deployment on CPU, keeps operator out of
component-qualified scoring Service, performs real `/v1/score`; never
replace it with health-only or reconciler behavior that production code does
not implement. Keep workflow and `docs/k8s/integration-tests.md` aligned.
Dependency-free `scripts/ci/test_e2e_runtime_contract.py` runs once, in
always-on Tooling Tests (tooling suite, ADR-1568); never move it solely
behind E2E schedule/label gate or back into workflow step. Cluster job
writes `VMAFX_E2E_KUBECONFIG` and `KUBECONFIG` to
same new file below `RUNNER_TEMP`; every Kubernetes step must first prove
exact `kind-${KIND_CLUSTER_NAME}` context and loopback API server. Teardown
must fail visibly if that identity guard cannot prove exact named cluster.

## Downloads bounded, diagnostics never silenced (HISS-02, HISS-07)

Every `curl` of `e2e-k8s.yml`'s tool installs carries `--connect-timeout 20
--max-time 300` next to its retries; never drop deadline. diagnostics
step runs each command through `diag()`, which prints `::warning::` on
failure and continues; never return to `|| true` or `2>/dev/null`, which hid
failures. Helm-chart workflow runs `test_helm_controller_workload.py`
and renders selector check with `controller.enabled` (ADR-1589).
