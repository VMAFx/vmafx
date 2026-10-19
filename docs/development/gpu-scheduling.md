<!-- markdownlint-disable MD060 -->
# GPU Scheduling in Kubernetes

Use this guide to install the GPU device-plugin for your vendor, to pick the
backend the chart selects, and to diagnose pods stuck in `Pending` for lack of
GPU resources. It maps each GPU vendor to a Kubernetes resource limit and a
VMAFX backend.

## Prerequisites

- A cluster with at least one GPU node and the vendor driver installed.
- Node labels from [Node Feature
  Discovery](https://kubernetes-sigs.github.io/node-feature-discovery/)
  (NFD) for Intel nodes, and from GPU Feature Discovery (GFD, shipped with the
  NVIDIA device plugin) if you use the `nvidia.com/gpu.present` affinity
  example below.
- Helm, to install the VMAFX chart (see the
  [Kubernetes deployment guide](k8s-deployment.md)).

## How GPU device-plugins work

A Kubernetes device-plugin is a daemonset that advertises custom extended
resources (e.g. `nvidia.com/gpu`) to the kubelet.  When a pod requests such a
resource, the scheduler places it on a node that has enough of that resource
available, and the kubelet allocates the physical device to the container.

VMAFX uses one device-plugin per GPU vendor:

| Vendor | Resource key | Backend | Plugin daemonset |
|---|---|---|---|
| NVIDIA | `nvidia.com/gpu` | CUDA | [k8s-device-plugin](https://github.com/NVIDIA/k8s-device-plugin) |
| AMD | `amd.com/gpu` | HIP | [k8s-device-plugin](https://github.com/ROCm/k8s-device-plugin) |
| Intel | `gpu.intel.com/i915` or `gpu.intel.com/xe` | SYCL | [intel-device-plugins-for-kubernetes](https://github.com/intel/intel-device-plugins-for-kubernetes) |

The Intel resource name depends on the GPU's kernel driver: the plugin
advertises `gpu.intel.com/i915` for the `i915` driver and `gpu.intel.com/xe`
for the `xe` driver, which Arc B-series (Battlemage) and newer GPUs use, and
which older GPUs use when bound to `xe`. Tell the chart which one with
`gpu.intelDriver` (default `i915`):

```yaml
gpu:
  vendor: intel
  intelDriver: xe     # requests gpu.intel.com/xe, e.g. Arc B580 / Pro B60
```

Check what a node advertises before choosing:

```bash
kubectl describe node <gpu-node> | grep "gpu.intel.com/"
```

`gpu.resourceName` requests any other extended resource verbatim, for any
vendor: a GPU sharing resource, an NVIDIA MIG slice
(`nvidia.com/mig-1g.10gb`), or a renamed plugin resource. It overrides
`gpu.vendor`'s default and `gpu.intelDriver`; the backend still follows
`gpu.vendor`. The chart refuses it with `gpu.vendor: cpu`, and refuses an
`intelDriver` other than `i915` or `xe`. See the plugin's
[GPU plugin README](https://github.com/intel/intel-device-plugins-for-kubernetes/tree/main/cmd/gpu_plugin)
for the resource names it advertises.

## Backend selection

Set `gpu.vendor` to the physical GPU vendor. The chart requests that vendor's
device-plugin resource and sets each node's `VMAFX_BACKEND` to `cuda`, `hip`,
`sycl`, or `cpu` accordingly. The scoring server takes its backend from each
request's `backend` score option.

The Vulkan backend was removed in
[ADR-0726](../adr/0726-drop-vulkan-backend.md).
It is not available through any vendor setting and the VMAFX images do not ship
it as a fallback backend.

## Installing device-plugins

### NVIDIA

```bash
kubectl apply -f \
  https://raw.githubusercontent.com/NVIDIA/k8s-device-plugin/v0.20.1/deployments/static/nvidia-device-plugin.yml
```

`v0.20.1` was the latest release when this page was checked; pin the release
you have validated against your driver version.

Verify:

```bash
kubectl get daemonset -n kube-system nvidia-device-plugin-daemonset
kubectl describe node <gpu-node> | grep -A 5 "nvidia.com/gpu"
```

### AMD (ROCm)

```bash
kubectl apply -f \
  https://raw.githubusercontent.com/ROCm/k8s-device-plugin/master/k8s-ds-amdgpu-dp.yaml
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

Verify (the resource is `gpu.intel.com/i915` or `gpu.intel.com/xe`; set
`gpu.intelDriver` to match):

```bash
kubectl describe node <gpu-node> | grep "gpu.intel.com/"
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

### `Insufficient gpu.intel.com/i915` or `gpu.intel.com/xe`

Same root causes as above, but for Intel. First check that `gpu.intelDriver`
matches what the node advertises: a node whose GPU runs on `xe` has no
`gpu.intel.com/i915`, and the reverse.  The Intel plugin additionally
requires the NFD (Node Feature Discovery) operator to label nodes correctly.
If the node is not labeled, the daemonset may not deploy onto it:

```bash
kubectl get node <node> --show-labels | grep "feature.node.kubernetes.io/kernel-module.i915"
```

### `Insufficient amd.com/gpu`

Same pattern.  Also check that the ROCm version installed on the node matches
what the device-plugin expects.

### GPU pod is running but VMAFX uses CPU

On a node, check that `VMAFX_BACKEND` is set correctly:

```bash
kubectl exec -n vmafx deploy/vmafx-node -- env | grep VMAFX_BACKEND
```

The scoring server runs the backend a request names in its `backend` score
option; a request without one runs the `vmaf` CLI's default.

If the value is `cpu` but `gpu.vendor` is set to a GPU vendor, verify the
device was actually allocated:

```bash
kubectl exec -n vmafx deploy/vmafx-node -- ls /dev/dri/
```

### Checking node GPU feature labels

```bash
kubectl get nodes --show-labels | grep -o "gpu\.[^,=]*=[^,]*"
```

## Node affinity and tolerations

GPU nodes are commonly tainted to prevent non-GPU pods from landing on them.
A typical NVIDIA taint: `nvidia.com/gpu=present:NoSchedule`. The
`nvidia.com/gpu.present` node label used below is set by GPU Feature
Discovery, not by the chart.

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
