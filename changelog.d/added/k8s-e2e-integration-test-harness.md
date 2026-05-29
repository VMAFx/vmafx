Add Kubernetes e2e integration test harness (`test/e2e/`) for the VMAFx Phase 4b
platform: `kind-cluster.sh` bootstraps a kind cluster with real or simulated GPU
support; five kuttl test cases cover operator installation, VmafxJob Pod creation,
VmafxNode heartbeat, rclone-backed scoring, and online sidecar trainer checkpoint
push. CI workflow runs nightly and on PRs with the `run-e2e-k8s` label.
(ADR-0783)
