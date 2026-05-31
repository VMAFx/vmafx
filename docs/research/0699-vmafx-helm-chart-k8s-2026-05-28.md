<!-- markdownlint-disable MD013 MD060 -->
# Research-0699: VMAFX Helm chart and Kubernetes GPU scheduling (2026-05-28)

Supporting investigation for ADR-0699.

## Summary

Investigated the correct Kubernetes resource keys for each GPU vendor's
device-plugin, the relationship between Vulkan and device-plugins, and the
correct Helm chart structure for three distinct VMAFX deployment patterns.

## GPU device-plugin resource key landscape

Each GPU vendor ships a separate Kubernetes device-plugin daemonset that
advertises custom extended resources to the kubelet via the
[Device Plugin API](https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/).

| Vendor | Resource key | Source |
|---|---|---|
| NVIDIA | `nvidia.com/gpu` | [k8s-device-plugin](https://github.com/NVIDIA/k8s-device-plugin) |
| AMD | `amd.com/gpu` | [ROCm k8s-device-plugin](https://github.com/RadeonOpenCompute/k8s-device-plugin) |
| Intel | `gpu.intel.com/i915` | [intel-device-plugins-for-kubernetes](https://github.com/intel/intel-device-plugins-for-kubernetes) |

Intel's plugin additionally requires NFD (Node Feature Discovery) to label
nodes before the daemonset will deploy onto them. This is a known operational
prerequisite documented in `gpu-scheduling.md`.

## Vulkan is not a separate Kubernetes resource

Confirmed via the Vulkan specification and vendor device-plugin source: there
is no `vulkan.khronos.org/gpu` or equivalent extended resource. Vulkan operates
as a graphics/compute API layered on top of whatever physical device the GPU
device-plugin allocates.

The Vulkan ICD (Installable Client Driver) selection happens at runtime via
`/etc/vulkan/icd.d/` or `VK_ICD_FILENAMES`. The VMAFX container image ships
the NVIDIA, AMD (AMDVLK), and Intel (ANV) Vulkan drivers; the correct one is
selected when the physical device is present in `/dev/dri/` after allocation.

This means:

- No separate `gpu.vulkan.count: 1` request is needed or possible.
- Vulkan backend availability follows GPU device-plugin availability.
- The chart need only request the vendor's GPU resource; Vulkan "comes along for free".

## Helm chart design decisions

### Conditional workload via single values key

Evaluated two approaches:

1. **Separate charts per workload** — clean, no conditionals; but triples
   maintenance burden and forces the operator to pick the right chart before
   understanding what they are deploying.

2. **Single chart with `workload: Deployment | Job | StatefulSet`** — the
   chosen approach. Helm's `{{- if eq ... }}` guards are idiomatic (used
   widely by Bitnami and Prometheus charts). One `values.yaml` captures the
   full config surface.

### GPU vendor selector in _helpers.tpl

The `vmafx.gpuResource` named template centralises the resource-key
resolution in one place (`_helpers.tpl`) so all three workload templates
(deployment, job, statefulset) can call `include "vmafx.gpuResource" .`
without repeating the if/else chain. The `vmafx.backendEnvValue` helper
follows the same pattern for `VMAFX_BACKEND`.

### Optional prometheus-pushgateway dependency

Job workloads cannot expose a long-lived scrape endpoint. The
prometheus-pushgateway is the standard pattern for batch jobs to push metrics
to Prometheus. Made conditional via `pushgateway.enabled: false` (default) so
the dependency does not impose a required scrape-endpoint setup on
Deployment/StatefulSet operators.

### Security context defaults

Applied hardened defaults (non-root UID 65534, read-only rootfs, all caps
dropped) per Kubernetes Pod Security Standards "Restricted" profile. These
defaults are overridable in `values.yaml` for environments that cannot satisfy
the restricted profile.

## Alternative: Kustomize

Kustomize is the other common Kubernetes templating approach.  Evaluated and
rejected for this use case because:

- No dependency management (`helm dependency build` handles pushgateway).
- No `helm lint` equivalent that catches template errors before cluster apply.
- Patch overlays for multi-vendor GPU variants would require one overlay per
  vendor, equivalent code volume to Helm conditionals but less readable.

## Helm lint results

`helm lint` passes with only an `[INFO]` about missing chart icon (cosmetic)
for all four vendor values (nvidia, amd, intel, cpu) and all three workload
types. `helm template` output validated via Python `yaml.safe_load_all` — 5
documents produced for default values, all parse cleanly.
