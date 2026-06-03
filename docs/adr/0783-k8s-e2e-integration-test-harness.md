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

We will ship `test/e2e/kind-cluster.sh` (idempotent kind cluster bootstrap with
real-NVIDIA or fake-GPU path via squat/k8s-fakedeviceplugin), `test/e2e/kuttl-tests/`
(five ordered kuttl test cases), and `.github/workflows/e2e-k8s.yml`. Documentation
lands in `docs/k8s/integration-tests.md`. This test surface is non-blocking on PRs
(opt-in label gate) until all five test cases pass reliably in CI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| kuttl (chosen) | Declarative YAML assertions; maintained by kube-burner community; no custom Go code | Requires kind + kubectl already present; sequential-only test ordering | Best balance of simplicity and real-k8s coverage |
| chainsaw (Kyverno's e2e tool) | Rich assertion DSL; supports parallel steps | Newer, smaller ecosystem; adds Kyverno dependency for a non-Kyverno project | Ecosystem risk; overkill for five sequential test cases |
| envtest (controller-runtime) | Pure Go; runs in-process; fast | Does not exercise Kubernetes networking, DNS, or admission controllers | In-process simulation misses the integration surface we need to test |
| Permanent cloud cluster (EKS / GKE) | Closest to production; tests real GPU scheduling | Cost; secret management; slow teardown; cluster drift | Cost prohibitive for nightly runs; kind achieves the same CRD/reconciliation coverage |

## Consequences

- **Positive**: Full controller → node → trainer loop is now automatically tested;
  regressions in CRD reconciliation are caught before merge; local developers
  can reproduce exactly with `bash test/e2e/kind-cluster.sh`.
- **Negative**: Nightly job adds ~15 min to CI wall time; fake-GPU path does not
  exercise CUDA kernels (GPU scoring in test case 04 uses CPU fallback).
- **Neutral / follow-ups**: Test case 05 (sidecar-trainer) requires the operator
  `currentSamples` increment logic to be implemented; the `required-aggregator.yml`
  should mark `E2E — Kubernetes Integration` as non-blocking until all five cases pass.

## References

- Open DRAFT PR: #152 (`feat(ci): k8s e2e integration test harness — kind + kuttl`).
- ADR-0709: Phase 4b distributed platform.
- ADR-0711: vmafx-controller implementation.
- ADR-0713: vmafx-node implementation.
- ADR-0781: sidecar SGD-EMA online trainer.
