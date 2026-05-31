<!-- markdownlint-disable MD013 MD060 -->
# ADR-0699: VMAFX Helm Chart and Kubernetes Manifests with 3-Vendor GPU Device-Plugin Support

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: deploy, kubernetes, helm, gpu, cuda, hip, sycl, vulkan, fork-local

## Context

The VMAFX fork rebrand (ADR-0686) and cloud-native redesign (ADR-0697)
identified Kubernetes deployment as a required delivery surface.  Operators
need a standardised way to deploy the VMAFX scoring server, one-shot batch
scoring jobs, and the MCP server inside Kubernetes clusters, with support for
all three GPU vendors that the fork already supports at the binary level
(NVIDIA/CUDA, AMD/HIP, Intel/SYCL).

Kubernetes GPU scheduling is mediated by vendor-specific device-plugin
daemonsets that advertise extended resources (e.g. `nvidia.com/gpu`,
`amd.com/gpu`, `gpu.intel.com/i915`).  A workload requests one of these
resources in its `resources.limits`; the scheduler places the pod on a
compatible node and the kubelet allocates the physical device.  Vulkan is not
a separate resource — it runs through whichever vendor's device is allocated.

Three distinct deployment patterns are required:

- **Long-running server** (Deployment): HTTP scoring endpoint; horizontal scale
  via replicas; rolling update.
- **One-shot batch job** (Job): `vmaf-tune compare` / `vmaf-tune ladder`; CI
  pipelines, nightly sweeps.
- **Stateful MCP server** (StatefulSet): sticky session state, stable DNS
  identity for socket-based MCP clients.

## Decision

Ship a Helm chart at `deploy/helm/vmafx/` with:

1. A `gpu` block in `values.yaml` that maps `vendor` (nvidia | amd | intel |
   cpu) to the correct Kubernetes extended-resource key via a
   `vmafx.gpuResource` named template in `_helpers.tpl`.

2. A companion `vmafx.backendEnvValue` helper that sets `VMAFX_BACKEND`
   automatically, so the VMAFX runtime picks the right backend without
   operator intervention.

3. Conditional workload templates (Deployment, Job, StatefulSet) selected via
   `values.workload`, sharing common pod-spec configuration.

4. A `prometheus-pushgateway` optional dependency for Job workloads that push
   metrics rather than exposing a scrape endpoint.

5. `helm lint` must pass with no errors; `helm template` output must be valid
   YAML for all four `gpu.vendor` values and all three `workload` types.

6. Human-readable documentation in `docs/development/k8s-deployment.md` and
   `docs/development/gpu-scheduling.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raw Kubernetes YAML (kustomize) | No Helm dependency; straightforward | No values templating, no `helm lint`, no dependency management; verbose for multi-vendor GPU logic | Helm is the de-facto operator standard and provides `upgrade --install` idempotency |
| Operator (controller-runtime) | Most powerful; CRD-driven lifecycle | Large scope, requires Go controller project, far exceeds the deployment requirement | Premature; a Helm chart is the right baseline; an operator can be layered later |
| Single workload type only | Simpler chart | Forces users to write custom manifests for batch or MCP scenarios | All three deployment patterns are user-discoverable and documented in ADR-0697 |
| Separate charts per vendor | Maximally explicit | Triplicates templates; GPU vendor is a runtime concern, not a chart concern | One chart with a vendor selector is idiomatic; keeps the operator mental model simple |

## Consequences

- **Positive**:
  - Operators install VMAFX on any GPU vendor with a single `helm upgrade --install` + `--set gpu.vendor=<vendor>`.
  - `VMAFX_BACKEND` is set automatically — no manual env-var wiring.
  - All three workload types (Deployment, Job, StatefulSet) are supported from day one.
  - Security context is hardened by default (non-root, read-only rootfs, all caps dropped).
  - `helm lint` passes with no errors for all vendor/workload combinations.

- **Negative**:
  - Helm dependency on `prometheus-pushgateway` requires `helm dependency build` before first install (even if pushgateway is disabled).
  - The chart ships without a real container image; operators must supply `image.repository` + `image.tag` matching their registry.

- **Neutral / follow-ups**:
  - A `values.schema.json` for strict schema validation can be added in a follow-up.
  - An Argo CD `Application` or Flux `HelmRelease` example can be added to docs.
  - A GitHub Actions workflow for `helm package` + OCI push to `ghcr.io/lusoris/charts` is a natural follow-up (ADR-0697 §Phase 2).

## References

- ADR-0697 (vmafx-cloud-native-redesign) — parent cloud-native umbrella
- ADR-0698 (vmafx-production-dockerfile) — sibling production container
- ADR-0686 (vmafx-rebrand-aggressive-modernization) — project rebrand umbrella
- req: user direction 2026-05-28 — Helm chart + K8s manifests with 3-vendor GPU device-plugin support
