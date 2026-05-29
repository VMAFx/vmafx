# ADR-0796: Helm Chart 3-Vendor GPU Scheduling — NodeAffinity, Tolerations, and Extension Model

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: deploy, kubernetes, helm, gpu, nvidia, amd, intel, scheduling, fork-local

## Context

ADR-0699 shipped the VMAFX Helm chart with a `gpu.vendor` selector that maps
to the correct Kubernetes extended-resource key (`nvidia.com/gpu`,
`amd.com/gpu`, `gpu.intel.com/i915`) and sets `VMAFX_BACKEND` automatically.
However, the chart's `affinity` and `tolerations` fields were left empty in
`values.yaml`, with only prose comments explaining what to put there.

In practice, GPU nodes in managed Kubernetes clusters (GKE, EKS, AKS) are
routinely tainted to prevent general-purpose workloads from occupying
expensive GPU capacity.  Without concrete nodeAffinity rules and matching
tolerations, a new operator faces a "pending" pod and must diagnose the
scheduling gap manually.  The ADR-0699 doc (`gpu-scheduling.md`) describes the
correct affinity and toleration shapes, but no concrete per-vendor values file
materialises them.

Three example values files are needed — one per supported GPU vendor — so
operators can install with a single `-f values-<vendor>.yaml` override and
immediately schedule on the right node class without reading scheduling
internals.

## Decision

1. **Three vendor example values files** are added at `deploy/helm/vmafx/`:
   - `values-nvidia.yaml` — CUDA backend; requires NVIDIA device-plugin;
     nodeAffinity on `nvidia.com/gpu.present: "true"`; toleration for
     `nvidia.com/gpu=present:NoSchedule`.
   - `values-amd.yaml` — HIP backend; requires ROCm device-plugin; nodeAffinity
     on `amd.com/gpu.present: "true"`; toleration for `amd.com/gpu=present:NoSchedule`.
   - `values-intel.yaml` — SYCL backend; requires Intel device-plugin + NFD;
     nodeAffinity on `feature.node.kubernetes.io/kernel-module.i915: "true"`;
     toleration for `gpu.intel.com/i915=present:NoSchedule`.

2. **Each example file also sets `node.tolerations` and `node.nodeSelector`**
   for the vmafx-node worker Deployment, so both the controller and the worker
   pool land on GPU nodes.

3. **The `values.yaml` default `affinity` and `tolerations` remain empty `{}`
   and `[]`.**  The defaults are for CPU workloads and general-purpose clusters
   where no taint is present.  Vendor-specific scheduling constraints are
   opt-in via the example files; operators with non-standard taints merge-override
   just the keys that differ.

4. **Extension model for a 4th vendor** (e.g. Qualcomm `qcom.com/gpu`, Apple
   MPS via a hypothetical `apple.com/gpu`):

   a. Add the resource key mapping in `_helpers.tpl` under `vmafx.gpuResource`
      and `vmafx.gpuResourceKey`.
   b. Add the backend string in `vmafx.backendEnvValue`.
   c. Add `values-<vendor>.yaml` with appropriate affinity key (vendor's
      device-plugin label or NFD label) and toleration (vendor's canonical
      taint key).
   d. Update `docs/development/gpu-scheduling.md` with the new vendor row.
   e. Add a `helm lint` smoke invocation in CI for the new vendor profile
      (`.github/workflows/helm-lint.yml`).
   f. Create an ADR (this one serves as the canonical reference for the
      extension pattern).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bake vendor affinity/tolerations into `_helpers.tpl` as auto-generated defaults based on `gpu.vendor` | Operator gets correct scheduling without any extra file | Silently forces affinity/tolerations on clusters with no GPU taints; breaks CPU deployments where no GPU label exists; hard to override for custom taint keys | The taint key and node label are cluster-specific; auto-generating them would cause phantom scheduling failures on non-standard setups |
| Add a `gpu.autoScheduling: true` flag that conditionally injects affinity/tolerations | Opt-in; explicit | Adds a boolean flag plus templating branch for each vendor (6 new template blocks); increases cognitive load for a feature that example files serve just as well | Example files are idiomatic Helm; they compose cleanly with `--set` and custom overrides without adding template complexity |
| Document-only (no example files, only prose) | Zero chart complexity | Operators must translate prose to YAML manually; first-run experience degrades; the ADR-0699 doc already provides prose without solving the UX gap | Example files are executable documentation; `helm template -f values-nvidia.yaml` is a direct smoke test |

## Consequences

- **Positive**:
  - Single-command install per vendor: `helm upgrade --install ... -f values-nvidia.yaml`.
  - `helm lint` passes on all three example files (verified with `helm lint -f
    values-<vendor>.yaml deploy/helm/vmafx/`).
  - `helm template` output for each vendor profile is valid YAML and contains
    the expected extended-resource key, nodeAffinity rule, and toleration.
  - The extension model is codified; adding a 4th vendor requires no chart
    redesign.

- **Negative**:
  - Node label keys (e.g. `amd.com/gpu.present`) are not guaranteed across
    all clusters; operators on clusters with different label conventions must
    override the affinity key.  The example files document the most common
    convention per vendor.

- **Neutral / follow-ups**:
  - A `values.schema.json` with an enum constraint on `gpu.vendor` values
    would surface unknown vendors at install time rather than at pod-scheduling
    time (ADR-0699 consequence, deferred).
  - A CI workflow (`helm-lint.yml`) that runs `helm lint -f values-<vendor>.yaml`
    for all three profiles on every PR touching `deploy/helm/` would catch
    template breakage early.

## References

- ADR-0699 — VMAFX Helm Chart initial design
- ADR-0697 — vmafx cloud-native redesign umbrella
- req: user direction 2026-05-29 — verify all 3 vendor paths work, add example values files, nodeAffinity rules, vendor tolerations, and ADR documenting 3-vendor model + how to add a 4th
