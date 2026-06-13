# Helm chart and Kubernetes manifests with 3-vendor GPU device-plugin support

Added `deploy/helm/vmafx/` — a production-ready Helm chart (apiVersion v2)
that supports three workload types (Deployment, Job, StatefulSet) and all
three GPU device-plugin vendors:

- NVIDIA → `nvidia.com/gpu` → `VMAFX_BACKEND=cuda`
- AMD → `amd.com/gpu` → `VMAFX_BACKEND=hip`
- Intel → `gpu.intel.com/i915` → `VMAFX_BACKEND=sycl`
- CPU → no device-plugin → `VMAFX_BACKEND=cpu`

The chart automatically sets `VMAFX_BACKEND` from `gpu.vendor`.  Vulkan is
not a separate Kubernetes resource; it runs through the allocated vendor's
device-plugin.

Documentation: `docs/development/k8s-deployment.md`,
`docs/development/gpu-scheduling.md`.  ADR-0699.
