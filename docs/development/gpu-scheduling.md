<!-- markdownlint-disable MD060 -->
# GPU Scheduling in Kubernetes

This guide explains how VMAFX maps GPU vendor device-plugins to Kubernetes
resource limits, how Vulkan fits into the picture, and how to diagnose
pending pods caused by insufficient GPU resources.

## How GPU device-plugins work

A Kubernetes device-plugin is a daemonset that advertises custom extended
resources (e.g. `nvidia.com/gpu`) to the kubelet.  When a pod requests such a
resource, the scheduler places it on a node that has enough of that resource
available, and the kubelet allocates the physical device to the container.

VMAFX uses one device-plugin per GPU vendor:

| Vendor | Resource key | Backend | Plugin daemonset |
|---|---|---|---|
| NVIDIA | `nvidia.com/gpu` | CUDA | [k8s-device-plugin](https://github.com/NVIDIA/k8s-device-plugin) |
| AMD | `amd.com/gpu` | HIP | [k8s-device-plugin](https://github.com/RadeonOpenCompute/k8s-device-plugin) |
| Intel | `gpu.intel.com/i915` | SYCL | [intel-device-plugins-for-kubernetes](https://github.com/intel/intel-device-plugins-for-kubernetes) |

## Vulkan and Kubernetes

**Vulkan is NOT a separate Kubernetes resource.** There is no
`vulkan.khronos.org/gpu` or equivalent extended resource in any vendor's
device-plugin.  Vulkan runs through whichever GPU device-plugin is allocated:

- NVIDIA node with `nvidia.com/gpu: 1` → Vulkan addresses the NVIDIA GPU via the
  NVIDIA Vulkan ICD.
- AMD node with `amd.com/gpu: 1` → Vulkan addresses the AMD GPU via the
  AMDVLK / Mesa RADV ICD.
- Intel node with `gpu.intel.com/i915: 1` → Vulkan addresses the Intel GPU via
  the Intel ANV / Mesa ANV ICD.

The VMAFX container image ships all three Vulkan ICDs.  The runtime selects the
correct ICD based on which device is present in `/dev/dri/` after the
device-plugin allocation.

**Consequence for the Helm chart:** set `gpu.vendor` to the physical GPU vendor.
The chart requests the vendor's device-plugin resource and sets `VMAFX_BACKEND`
accordingly.  Vulkan acceleration is available automatically on any allocated
GPU node without a separate resource request.

## Installing device-plugins

### NVIDIA

```bash
kubectl apply -f \
  https://raw.githubusercontent.com/NVIDIA/k8s-device-plugin/v0.14.5/nvidia-device-plugin.yml
```

Verify:

```bash
kubectl get daemonset -n kube-system nvidia-device-plugin-daemonset
kubectl describe node <gpu-node> | grep -A 5 "nvidia.com/gpu"
```

### AMD (ROCm)

```bash
kubectl apply -f \
  https://raw.githubusercontent.com/RadeonOpenCompute/k8s-device-plugin/master/k8s-ds-amdgpu-dp.yaml
```

Verify:

```bash
kubectl describe node <gpu-node> | grep "amd.com/gpu"
```

### Intel

```bash
# Requires the Intel Device Plugins Operator or manual daemonset deploy.
# See: https://github.com/intel/intel-device-plugins-for-kubernetes/tree/main/cmd/gpu_plugin
kubectl apply -k \
  https://github.com/intel/intel-device-plugins-for-kubernetes/deployments/gpu_plugin/overlays/nfd_labeled_nodes
```

Verify:

```bash
kubectl describe node <gpu-node> | grep "gpu.intel.com/i915"
```

## Node capacity and allocatable

Check what GPU resources a node is advertising:

```bash
kubectl describe node <node-name> | grep -E "Capacity|Allocatable" -A 15
```

Example output for an NVIDIA node:

```text
Capacity:
  ...
  nvidia.com/gpu:     1
Allocatable:
  ...
  nvidia.com/gpu:     1
```

If the capacity shows 0 or the key is absent, the device-plugin is either
not installed or the node does not have a compatible GPU.

## Troubleshooting pending pods

### `Insufficient nvidia.com/gpu`

```text
0/3 nodes are available: 3 Insufficient nvidia.com/gpu.
```

Causes and fixes:

1. **Device-plugin not installed.** Install the NVIDIA device-plugin daemonset.
2. **Node is tainted but pod has no toleration.** Add a toleration:

   ```yaml
   tolerations:
     - key: nvidia.com/gpu
       operator: Exists
       effect: NoSchedule
   ```

3. **All GPUs already allocated.** Reduce `gpu.count`, free other pods, or add
   a GPU node.
4. **Pod is requesting more GPUs than available.**

   ```bash
   kubectl get pod <pod> -o jsonpath='{.spec.containers[0].resources}'
   ```

### `Insufficient gpu.intel.com/i915`

Same root causes as above, but for Intel.  The Intel plugin additionally
requires the NFD (Node Feature Discovery) operator to label nodes correctly.
If the node is not labeled, the daemonset may not deploy onto it:

```bash
kubectl get node <node> --show-labels | grep "feature.node.kubernetes.io/kernel-module.i915"
```

### `Insufficient amd.com/gpu`

Same pattern.  Also check that the ROCm version installed on the node matches
what the device-plugin expects.

### GPU pod is running but VMAFX uses CPU

Check that `VMAFX_BACKEND` is set correctly:

```bash
kubectl exec -n vmafx deploy/vmafx -- env | grep VMAFX_BACKEND
```

If the value is `cpu` but `gpu.vendor` is set to a GPU vendor, verify the
device was actually allocated:

```bash
kubectl exec -n vmafx deploy/vmafx -- ls /dev/dri/
```

### Checking node GPU feature labels

```bash
kubectl get nodes --show-labels | grep -o "gpu\.[^,=]*=[^,]*"
```

## Node affinity and tolerations

GPU nodes are commonly tainted to prevent non-GPU pods from landing on them.
A typical NVIDIA taint: `nvidia.com/gpu=present:NoSchedule`.

To ensure VMAFX is scheduled on GPU nodes:

```yaml
# values.yaml
tolerations:
  - key: nvidia.com/gpu
    operator: Exists
    effect: NoSchedule

affinity:
  nodeAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      nodeSelectorTerms:
        - matchExpressions:
            - key: nvidia.com/gpu.present
              operator: In
              values: ["true"]
```

## Multi-GPU nodes

To request more than one GPU per pod:

```bash
helm upgrade vmafx deploy/helm/vmafx/ --set gpu.count=2
```

Note that VMAFX processes a single job per pod; multiple GPUs per pod are only
useful if the VMAFX backend supports intra-node multi-GPU dispatch.

## Related

- [Kubernetes deployment guide](k8s-deployment.md)
- [Backend documentation](../backends/index.md)
- [ADR-0699](../adr/0699-vmafx-helm-chart-k8s.md) — Helm chart design
