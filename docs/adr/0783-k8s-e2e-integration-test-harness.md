<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0783: Kubernetes end-to-end integration test harness — kind + kuttl

- **Status**: Proposed
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `testing`, `k8s`, `github`

## Context

The Phase 4b platform (ADR-0709) ships a Kubernetes Operator, Node worker, and sidecar
trainer that interact through CRDs and a gRPC control plane. Unit tests exercise
individual components in isolation, but there is no test that validates the full
controller → node → trainer loop on real Kubernetes. This gap means regressions in CRD
reconciliation, job dispatch, or cross-component communication go undetected until a
live deployment.

A lightweight end-to-end harness using kind (Kubernetes in Docker) eliminates the need
for a permanent cloud cluster while still exercising real Kubernetes APIs. kuttl
(KUbernetes Test TooL) provides a declarative YAML-based assertion layer that is
easier to maintain than raw Go integration tests.

The five test cases cover: CRD installation, VmafxJob pod lifecycle, VmafxNode
heartbeat, rclone-sourced scoring, and the sidecar trainer checkpoint flow.

A nightly CI workflow (`.github/workflows/e2e-k8s.yml`) runs the harness at 03:47 UTC
and is also opt-in on PRs via a `run-e2e-k8s` label. An 8-frame 64×64 YUV420p fixture
pair in `test/e2e/fixtures/` allows deterministic scoring without network access.

## Decision

We will add a `test/e2e/` directory containing:

1. `kind-cluster.sh` — idempotent cluster bootstrap with real-GPU (NVIDIA device
   plugin) or simulated-GPU (fake-device-plugin DaemonSet) paths, cert-manager,
   and CRD installation via the existing `deploy/helm/vmafx` chart.
2. `kuttl-tests/` — five ordered kuttl test cases:
   - `01-operator-installs`: CRD establishment + operator `Deployment` available.
   - `02-vmafxjob-creates-pod`: VmafxJob CR triggers a worker Pod and reaches
     `Succeeded` phase with a populated score.
   - `03-node-heartbeat`: VmafxNode CR backed by a stub `/healthz` server
     receives a `lastHeartbeat` timestamp from the operator's probe loop.
   - `04-rclone-score`: End-to-end rclone-fetch → vmaf-score path via an
     in-cluster MinIO stand-in.
   - `05-sidecar-trainer`: VmafxModelTraining CR reaches `Running`, ingests one
     feedback sample, and pushes a checkpoint to an in-cluster OCI registry stub.
3. `.github/workflows/e2e-k8s.yml` — nightly schedule (03:47 UTC) plus opt-in
   on PRs via the `run-e2e-k8s` label; skipped by default on PRs to keep CI fast.
4. `test/e2e/fixtures/gen-tiny-yuv.sh` — generates committed 64×64 8-frame
   YUV420p clips for deterministic scoring without network fetches.
5. `docs/k8s/integration-tests.md` — operator guide for running the suite locally
   and interpreting results.

kuttl is chosen over raw shell scripts because it provides declarative YAML
assert semantics, built-in retry/timeout, per-step artifact collection, and XML
test reporting consumable by the existing `publish-unit-test-result-action`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| kuttl (chosen) | Declarative YAML assertions; maintained by kube-burner community; no custom Go code | Requires kind + kubectl already present; sequential-only test ordering | Best balance of simplicity and real-k8s coverage |
| Chainsaw (kuttl successor) | More expressive; better error messages; richer assertion DSL | Less mature; different YAML schema; ecosystem risk | Could migrate later once Chainsaw stabilises |
| chainsaw (Kyverno's e2e tool) | Rich assertion DSL; supports parallel steps | Newer, smaller ecosystem; adds Kyverno dependency for a non-Kyverno project | Ecosystem risk; overkill for five sequential test cases |
| envtest (controller-runtime) | Pure Go; runs in-process; fast | Does not exercise Kubernetes networking, DNS, or admission controllers | In-process simulation misses the integration surface we need to test |
| Permanent cloud cluster (EKS / GKE) | Closest to production; tests real GPU scheduling | Cost; secret management; slow teardown; cluster drift | Cost prohibitive for nightly runs; kind achieves the same CRD/reconciliation coverage |

## Consequences

- **Positive**: Full controller → node → trainer loop is now automatically tested;
  regressions in CRD reconciliation are caught before merge; local developers
  can reproduce exactly with `bash test/e2e/kind-cluster.sh`.
- **Positive**: Nightly run on GitHub-hosted runners with no GPU hardware; the
  fake-device-plugin makes `nvidia.com/gpu` resource requests schedulable in CI.
- **Positive**: PR opt-in via label keeps normal PR CI latency unchanged.
- **Negative**: Nightly job adds ~15–30 min to CI wall time; fake-GPU path does not
  exercise CUDA kernels (GPU scoring in test case 04 uses CPU fallback).
- **Negative**: kind cluster bootstrap adds a new failure mode (cert-manager pull,
  device plugin DaemonSet scheduling) unrelated to VMAFx code itself; flakiness
  must be tracked and suppressed with retries if needed.
- **Neutral / follow-ups**: Test case 05 (sidecar-trainer) requires the operator
  `currentSamples` increment logic to be implemented; the `required-aggregator.yml`
  should mark `E2E — Kubernetes Integration` as non-blocking until all five cases pass.

## References

- Open DRAFT PR: #152 (`feat(ci): k8s e2e integration test harness — kind + kuttl`).
- ADR-0709: Phase 4b distributed platform.
- ADR-0711: vmafx-controller implementation.
- ADR-0713: vmafx-node implementation.
- ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
- ADR-0781: sidecar SGD-EMA online trainer.
- ADR-0698: VMAFX production Dockerfile (Dockerfile.operator/Dockerfile.node TBD).
- kuttl documentation: <https://kuttl.dev/docs/>
- kind documentation: <https://kind.sigs.k8s.io/>
- fake-device-plugin: <https://github.com/squat/k8s-fakedeviceplugin>
- req: "Build a k8s integration test harness for the VMAFx Phase 4b platform."
