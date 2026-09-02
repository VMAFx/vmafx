<!-- markdownlint-disable MD013 MD060 -->
# VMAFX Phase 4b — Distributed Platform Architecture

> **Status**: Proposed (2026-05-28). Locks when ADR-0709 moves to Accepted.
> See [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) for the full
> decision record.

This document describes the target component layout introduced in Phase 4b.
The diagram below replaces the single-binary model from Phase 3 (ADR-0701) and
Phase 4a (ADR-0702) with a controller/node/operator split designed for
horizontal scale on heterogeneous GPU clusters.

## Component diagram (Mermaid)

```mermaid
graph TB
    subgraph "Thin clients"
        CLI["vmafx CLI\n(Go)"]
        MCP["vmafx-mcp\n(Go / JSON-RPC)"]
        Tune["vmafx-tune\n(Go)"]
    end

    subgraph "Control plane"
        CTRL["vmafx-controller\n(Go)\ngRPC + HTTP API\nJob queue · Node registry\nScheduler · /metrics · /healthz"]
        OP["vmafx-operator\n(Go · controller-runtime)\nCRDs: VmafxJob\nVmafxNode\nVmafxModelTraining\nHPA reconciler"]
    end

    subgraph "Worker pool — NVIDIA nodes"
        NODE_NV["vmafx-node\n(Go · CUDA EP)\nlibvmaf cgo\nffmpeg subprocess\nGo ONNX Runtime\nrclone mount"]
        SC_NV["training-sidecar\n(Python · PyTorch + Lightning)\ncontinuous fine-tune\nv1: per-node sidecar"]
    end

    subgraph "Worker pool — AMD nodes"
        NODE_AMD["vmafx-node\n(Go · ROCm EP)\nlibvmaf cgo\nffmpeg subprocess\nGo ONNX Runtime\nrclone mount"]
        SC_AMD["training-sidecar\n(Python · PyTorch + Lightning)"]
    end

    subgraph "Worker pool — Intel nodes"
        NODE_INT["vmafx-node\n(Go · OpenVINO EP)\nlibvmaf cgo\nffmpeg subprocess\nGo ONNX Runtime\nrclone mount"]
    end

    subgraph "Storage"
        S3["Object store\nS3 / GCS / Azure Blob\nSFTP / SSH\n(via rclone-mount)"]
        MODELREG["model/ registry\n.onnx + registry.json"]
    end

    subgraph "Kubernetes"
        K8S["k8s API server\nCRD watch"]
    end

    CLI -->|gRPC| CTRL
    MCP -->|gRPC| CTRL
    Tune -->|gRPC| CTRL

    CTRL -->|work items| NODE_NV
    CTRL -->|work items| NODE_AMD
    CTRL -->|work items| NODE_INT

    OP -->|reconcile| K8S
    K8S -->|CRD events| OP
    OP -->|pod lifecycle| CTRL

    NODE_NV -->|triples\n(ref,dis,score,meta)| SC_NV
    NODE_AMD -->|triples| SC_AMD

    SC_NV -->|updated .onnx| MODELREG
    SC_AMD -->|updated .onnx| MODELREG

    NODE_NV -->|rclone-vfs read| S3
    NODE_AMD -->|rclone-vfs read| S3
    NODE_INT -->|rclone-vfs read| S3

    NODE_NV -->|ONNX load| MODELREG
    NODE_AMD -->|ONNX load| MODELREG
    NODE_INT -->|ONNX load| MODELREG
```

## Component responsibilities

| Component | Language | Image | Key responsibilities |
|---|---|---|---|
| `vmafx-controller` | Go | distroless/cc | gRPC + HTTP API, job queue, node registry, scheduler, `/healthz /readyz /metrics` |
| `vmafx-operator` | Go (controller-runtime) | distroless/cc | Watches `VmafxJob` / `VmafxNode` / `VmafxModelTraining` CRDs; reconciles pod lifecycle; drives HPA |
| `vmafx-node` | Go | distroless/cc + ffmpeg + rclone | Pulls work, runs ffmpeg subprocess, scores via libvmaf cgo, AI inference via Go ONNX Runtime, captures training triples |
| `training-sidecar` | Python (PyTorch + Lightning) | pytorch base | Consumes `(ref, dis, score, metadata)` triples from co-located node; continuously fine-tunes ONNX model; writes updated `.onnx` to model registry |
| `vmafx-mcp` | Go | distroless/cc | MCP JSON-RPC server; delegates scoring to controller gRPC API |
| `vmafx-tune` | Go | distroless/cc | Encoder-ladder optimizer; submits jobs to controller |
| `vmafx` CLI | Go | — (binary) | User-facing CLI; submits jobs to controller |

## CRD summary

| CRD | Group | Scope | Purpose |
|---|---|---|---|
| `VmafxJob` | `vmafx.io/v1alpha1` | Namespaced | Describes a scoring / encoding / QA job (source, models, encoder params, target node pool) |
| `VmafxNode` | `vmafx.io/v1alpha1` | Cluster | Describes a node pool (GPU vendor, count, image, resource limits) |
| `VmafxModelTraining` | `vmafx.io/v1alpha1` | Namespaced | Describes a sidecar training run (base model, training config, output target) |

## Storage flow (zero-copy via rclone)

```text
Object store (S3 / GCS / Azure Blob / SFTP)
        │
        │  rclone mount / rclone-vfs (FUSE)
        ▼
  /mnt/source/   (inside vmafx-node pod)
        │
        │  POSIX read — no intermediate disk write
        ├──► ffmpeg subprocess (encode → encoded stream)
        └──► libvmaf cgo (score → result JSON)
```

## GPU pool affinity

Node pods are scheduled via k8s `nodeSelector` / `nodeAffinity` resource keys:

| Vendor | Resource key | Backend |
|---|---|---|
| NVIDIA | `nvidia.com/gpu` | CUDA EP |
| AMD | `amd.com/gpu` | ROCm EP + HIP |
| Intel | `gpu.intel.com/i915` | OpenVINO EP + SYCL |

Each backend runs through whichever GPU device plugin is allocated to the pod
(per ADR-0701). (The Vulkan backend was removed in ADR-0726.)

## Phase 4b sweep sequence

| Phase | Description | Input dependency |
|---|---|---|
| 4b.1 | `vmafx-server` → `vmafx-controller` (job queue, node registry, scheduler) | Phase 4a vmafx-server PR merged |
| 4b.2 | `vmafx-node` Go binary (libvmaf cgo, ffmpeg, Go ONNX Runtime) | vmafx-sys Rust bindings (Phase 4a) |
| 4b.3 | `vmafx-operator` kubebuilder skeleton + CRDs | Phase 4b.1 |
| 4b.4 | ffmpeg latest + `ffmpeg-patches/` bundled in node image | Phase 4b.2 |
| 4b.5 | rclone integration (node distroless layer + mount lifecycle) | Phase 4b.2 |
| 4b.6 | eBPF research digest + ONE concrete optimization | Phase 4b.2 (baseline measurement) |
| 4b.7 | Sidecar training v1 (Python sidecar + triple-capture API) | Phase 4b.2 + Phase 4b.3 |
| 4b.8 | C ABI break + ffmpeg-patches update | Phase 4b.4 |
| 4b.9 | Native build sunset (Docker + Helm only release artifacts) | Phase 4b.8 |

## Related documents

- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — umbrella decision record
- [ADR-0702](../adr/0702-vmafx-phase4-language-modernization.md) — Phase 4a foundation
- [ADR-0701](../adr/0701-vmafx-cloud-native-redesign.md) — Phase 3 cloud-native redesign
- [ADR-0699](../adr/0699-vmafx-helm-chart-k8s.md) — Helm chart + k8s manifests
- [c4-container.md](c4-container.md) — C4 Level 2 container view (to be updated as
  Phase 4b components stabilize)
