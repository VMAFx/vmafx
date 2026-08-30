<!-- markdownlint-disable MD013 MD060 -->
# ADR-0709: VMAFX Phase 4b — Distributed Video-Quality, Encoding, and ML Platform

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `architecture`, `go`, `k8s`, `operator`, `controller`, `node`, `ffmpeg`, `rclone`, `ebpf`, `onnx`, `training`, `abi`, `platform`, `phase4b`, `fork-local`

## Context

VMAFX reached a phase-transition point with Phase 4a (ADR-0702): the repository now has
Go and Rust workspaces, C++23 migration policy, and the MCP server, vmafx-server,
vmafx-mcp, and vmafx-tune binaries in development as independent Go modules.

Phase 4b is the architectural pivot from *single-binary scoring tool* to *distributed
video-quality, encoding, and ML platform*. The forces driving this pivot:

- **Scale requirement**: batch scoring sweeps (CHUG, K150K, BVI-DVC) currently run as
  long-lived single processes; horizontal scaling requires a controller/worker split.
- **Heterogeneous GPU pools**: the fork already supports CUDA, SYCL, HIP, Vulkan, and
  Metal backends. Scheduling work to the right GPU vendor pool requires a cluster-aware
  orchestration layer — not ad-hoc `--backend` flags.
- **Online training demand**: encoding a video is the ideal moment to collect
  `(ref, dis, score, metadata)` triples for continuous model refinement. The existing
  Python-only offline training loop in `ai/` cannot consume real-time encoder output.
- **Storage costs**: materializing full YUV frames to disk before scoring defeats the
  point of cloud-native deployment. A zero-copy storage layer (rclone-mount) eliminates
  the intermediate disk write.
- **Platform ambition**: VMAFX targets production video-quality measurement at CDN scale
  — encoder-ladder tuning, batch transcoding QA, and real-time quality monitoring. None
  of these use cases fit the single-binary model.

Phase 4b defines the target architecture. The in-flight Phase 4a agents (vmafx-server,
vmafx-mcp, vmafx-tune, vmafx-sys Rust bindings, C++23 internals) finish first; Phase 4b
layers on top of their output.

## Decision

We will transform VMAFX into a cloud-native distributed platform with the following
components:

1. **`vmafx-controller`** (Go) — the cluster brain. Exposes gRPC + HTTP API, owns the
   job queue, node registry, and work scheduler. Exposes `/healthz`, `/readyz`,
   `/metrics` (Prometheus). The existing `vmafx-server` (in-flight Phase 4a agent) is
   renamed and extended with controller-specific scope (job queue, node registry) in
   Phase 4b.1.

2. **`vmafx-node`** (Go) — the execution worker. Pulls work items from the controller,
   runs encoders (via ffmpeg subprocess), scores via libvmaf (cgo against
   `bindings/rust/vmafx-sys`), runs AI inference via Go ONNX Runtime (`onnxruntime-go`
   with CUDA EP + ROCm EP + OpenVINO EP). Reports results back to the controller. GPU
   pool affinity via k8s `nodeSelector` / `nodeAffinity` (vendor-keyed:
   `nvidia.com/gpu`, `amd.com/gpu`, `gpu.intel.com/i915`).

3. **`vmafx-operator`** (Go, `controller-runtime` / kubebuilder) — the Kubernetes
   Operator. Watches the `VmafxJob`, `VmafxNode`, and `VmafxModelTraining` CRDs,
   reconciles pod lifecycle, scales nodes via HPA against queue depth. Deployed alongside
   the controller in the Helm chart (ADR-0699).

4. **Thin clients** — `vmafx-mcp` and `vmafx-tune` (in-flight Phase 4a agents) are
   rewired to talk to the controller's gRPC API instead of running libvmaf directly.

5. **ffmpeg integration** — ffmpeg (latest pinned release) is bundled into the
   `vmafx-node` distroless image. Encoding is done via ffmpeg subprocess; scoring via
   libvmaf cgo directly. The existing `ffmpeg-patches/` stack continues to apply inside
   the container.

6. **rclone storage** — rclone is bundled into the node image. The node mounts the
   source bucket at `/mnt/source` via `rclone mount` or `rclone-vfs`, exposing a POSIX
   view of S3 / GCS / Azure Blob / SSH / SFTP. ffmpeg and libvmaf read directly from the
   mount; no intermediate disk materialization.

7. **eBPF optimizations** — research-first. A research digest identifies ONE concrete
   eBPF optimization target (I/O hot path, scheduling signal, XDP gRPC acceleration, or
   profiling) with measurable baseline before any implementation PR ships.

8. **AI inference in the node** — Go ONNX Runtime (`onnxruntime-go`) inside
   `vmafx-node`. Single Go binary, GPU-aware (CUDA EP, ROCm EP, OpenVINO EP). Same
   image runs scoring and AI inference. Training continues in `ai/` (Python / PyTorch +
   Lightning) for now.

9. **Sidecar training** — Python sidecar container (v1) co-located with each node.
   The Go node captures `(ref, dis, score, metadata)` triples and ships them to the
   Python sidecar for continuous model refinement via the existing PyTorch + Lightning
   stack. A dedicated `vmafx-training-node` pool (v2) is deferred until scale demands it.

10. **C ABI break** — the libvmaf public C API is no longer preserved as a stable
    external contract. The fork rewrites the public API surface toward C++23, Rust, and
    Go bindings. The in-tree `ffmpeg-patches/` stack is updated in the same PR to consume
    the new API. Downstream consumers external to this repository are not a constraint.

11. **Native build scope tightening** — libvmaf continues to exist inside the container
    (the Go node loads it via cgo). The `ffmpeg-patches/` stack continues to apply
    against ffmpeg-in-container. External publication of native `.so` / `.deb` / `.rpm`
    packages is intentionally dropped. The user-facing release artifacts are Docker
    images and the Helm chart only.

## Alternatives considered

### Architecture pattern

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single-binary with goroutines | Zero operational complexity; straightforward Go concurrency | No horizontal scale; GPU pool affinity impossible; no k8s lifecycle control | Does not meet scale or multi-vendor GPU requirements |
| Simple job queue (Redis/NATS) + workers | Lower operational overhead than a full Operator | No native k8s integration; requires external queue infrastructure; no CRD-based lifecycle | Adding an Operator on top later is harder than starting with one |
| **Controller/node + custom k8s Operator** (chosen) | Native k8s experience; CRD lifecycle; HPA; GPU nodeSelector per vendor; Helm-bundled | More moving parts; kubebuilder learning curve | Best fit for production multi-GPU multi-tenant cluster deployment |
| Upstream solutions (Argo Workflows, Tekton) | Mature; avoid custom Operator code | Heavyweight; require significant platform investment; do not understand VMAFX-specific CRDs | Per user direction: "of course this has to be fully connected to a ffmpeg worker as well" — bespoke CRDs are necessary |

### ffmpeg integration

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Separate `vmafx-ffmpeg-node` worker type | Clean separation; can scale encoder fleet independently | Two worker types to manage; controller complexity | Fold into standard node for v1; split later if scale demands it |
| **Fold ffmpeg into `vmafx-node` runtime** (chosen) | Single image; simpler scheduler; ffmpeg and libvmaf share the same process lifecycle | Node image is larger | Simpler v1; per user: "latest of course" ffmpeg pinned |
| Use libavcodec directly instead of ffmpeg subprocess | No subprocess overhead | Significant C complexity; lose ffmpeg filter graph | ffmpeg subprocess reuses the existing `ffmpeg-patches` integration path |

### Storage layer

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mount k8s PersistentVolumeClaim | Simple; native k8s | Requires CSI driver + provisioner per cloud; forces full materialization to PVC | Cloud-provider lock-in; no zero-copy |
| Copy files to node ephemeral storage | Simplest code path | Wastes disk; wastes cluster I/O budget; not viable at CHUG scale | Not zero-copy |
| **rclone-mount / rclone-vfs** (chosen) | Zero-copy POSIX view; supports S3, GCS, Azure Blob, SSH, SFTP; one integration point | Adds rclone binary to node image; FUSE mount lifecycle complexity | Per user direction and per recommendation: "use rclone for using files without copying to disk/RAM first" |

### eBPF scope

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Skip eBPF entirely | No kernel-level complexity | Leaves observable performance on the table | User explicitly requested eBPF: "if possible do ebpf optimizations" |
| Implement all eBPF use cases (XDP, scheduling, profiling) at once | Maximum optimization coverage | Massive scope; research territory; high risk of over-engineering | Research-first: measure baseline, identify one target, ship one PR |
| **Research-first: one concrete target** (chosen) | Controlled scope; measurable baseline; reversible | Defers potential gains | Best practice for kernel-level work; per memory file |

### AI inference in the node

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Python subprocess for ONNX inference | Reuses existing ai/ Python stack | Per-frame subprocess overhead; two process runtimes per node | Unacceptable latency for real-time scoring |
| C-cgo calling libvmaf DNN path directly | Minimal new dependencies | Bypasses Go-native EP selection; CUDA/ROCm EP harder to wire | Go ONNX Runtime has native EP support |
| **Go ONNX Runtime (`onnxruntime-go`)** (chosen) | Single Go binary; GPU EP selection (CUDA, ROCm, OpenVINO) native; same image runs scoring + inference | `onnxruntime-go` is less mature than C/Python ORT | Per user popup answer: "Inside vmafx-node via Go ONNX Runtime (Recommended)" |

### Sidecar training

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Python sidecar container per node** (chosen v1) | Reuses existing PyTorch + Lightning stack; pragmatic; no new ML framework | Requires sidecar container lifecycle management; data shipping between Go node and Python sidecar | Chosen for v1; per user: "ml training in python only is wrong — we want sidecar training while encoding" |
| Go-native online learning (Gorgonia / pure-Go SGD) | Single binary; no Python dependency | Go ML ecosystem not viable for full PyTorch fine-tune path | Not viable for full model training |
| Dedicated `vmafx-training-node` pool | Cleanest separation; dedicated GPU + PyTorch | More moving parts; overkill for v1 scale | Deferred to v2 per recommendation |

### C ABI break

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Preserve libvmaf public C ABI | Downstream consumers (FFmpeg mainline, GStreamer, third-party) remain unbroken | Constrains C++23 rewrite; prevents idiomatic Go/Rust/C++ surface | Per user direction: "we rewrite and we update the patches and then we are fine? because I don't care about what others do with my project or not" |
| **Break ABI; update ffmpeg-patches** (chosen) | Enables idiomatic C++23 + Rust + Go public surface; no legacy compatibility debt | ffmpeg-patches must be updated in the same PR; one-time migration cost | External downstream consumers are not a constraint for this fork |

### Native build publishing

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Continue publishing `.deb`/`.rpm`/`.so` | Wider reach for non-k8s users | Maintenance overhead; diverges from container-first mental model | Per user direction: "now that I concentrate on docker images/k8s, do we still need to build native things?" — external packages intentionally dropped |
| **Docker images + Helm chart only** (chosen) | Single release artifact type; aligns with container-first model | No native package install path | The Go node requires libvmaf inside the container anyway; external packages add cost with no user base |

## Implementation plan

The following phase sequence produces mergeable PRs in dependency order.
Each phase produces its own ADR + PR. This ADR is the umbrella.

| Phase | Title | Scope |
|---|---|---|
| **4b.1** | `vmafx-server` → `vmafx-controller` | Rename post-Phase-4a vmafx-server binary; add job queue (Redis or k8s Job CRDs), node registry, scheduler API |
| **4b.2** | `vmafx-node` Go binary | New `cmd/vmafx-node/` module; libvmaf cgo via vmafx-sys, ffmpeg subprocess, Go ONNX Runtime inference, result reporting |
| **4b.3** | `vmafx-operator` kubebuilder skeleton | `kubebuilder init` + CRDs: `VmafxJob`, `VmafxNode`, `VmafxModelTraining`; stub reconcile loops; RBAC |
| **4b.4** | ffmpeg latest bundled in node layer | Pin latest ffmpeg release; apply `ffmpeg-patches/` series inside container build; distroless node image update |
| **4b.5** | rclone integration | Bundle rclone in node distroless layer; `rclone mount` source bucket at `/mnt/source`; investigate `rclone-vfs` for true streaming |
| **4b.6** | eBPF research digest + ONE optimization | Research digest: measure baseline (I/O hot path / scheduling / XDP); select one concrete target; implement; gate on measurable improvement |
| **4b.7** | Sidecar training v1 | Python sidecar container spec; Go node triple-capture API; sidecar PyTorch + Lightning continuous training loop; CRD `VmafxModelTraining` reconciler |
| **4b.8** | C ABI break + ffmpeg-patches update | Public API rewrite to C++23 + Rust + Go surface; `ffmpeg-patches/` series updated to consume new API in the same PR |
| **4b.9** | Native build sunset | Release pipeline: publish Docker images + Helm chart only; drop `.deb` / `.rpm` / `.so` publication steps |

## Out of scope

This ADR does NOT cover:

- **Specific model architectures** — handled by per-model ADRs (e.g., ADR-0682, future
  tiny-AI model ADRs).
- **eBPF specifics** — the concrete optimization target, kernel program design, and
  performance gate are all handled in the Phase 4b.6 research digest and its
  accompanying ADR.
- **Sidecar training algorithm choice** — architecture selection (Python sidecar v1 vs
  dedicated training nodes v2), loss function, fine-tune strategy, and data schema are
  handled by the Phase 4b.7 research digest.
- **External native package publishing** — intentionally out of scope per user direction.
  No `.deb`, `.rpm`, or standalone `.so` publication path. This is a permanent removal,
  not a deferral.
- **Netflix pipeline function backlog** — a parallel research digest will inventory
  Netflix-upstream functions not yet ported to the fork and produce a porting backlog.
  That backlog integrates into the Phase 4b workstream but is its own research artifact.
- **Helm chart GPU node pool specifics** — per-vendor `nodeSelector` details and HPA
  thresholds are handled inside Phase 4b.3 and the Phase 3 Helm chart ADR-0699.

## Consequences

**Positive:**

- Horizontal scaling: add `vmafx-node` pods to process more jobs in parallel.
- k8s-native deployment: CRDs, RBAC, HPA, Helm chart — standard operator pattern.
- GPU-vendor-agnostic pools: controller dispatches to NVIDIA / AMD / Intel nodes by
  vendor-keyed nodeSelector; same job definition runs on any pool.
- Online learning: sidecar training closes the encode → score → train loop; model
  quality improves continuously as the platform processes real workloads.
- Zero-copy storage: rclone-mount eliminates intermediate disk writes; significant I/O
  cost reduction at CHUG / K150K / BVI-DVC scale.
- Idiomatic multi-language public surface: C++23 + Rust + Go instead of a C11 API
  frozen for downstream compatibility reasons.

**Negative:**

- Significant engineering effort: controller, node, and operator are three new Go
  binaries with distinct responsibilities.
- ffmpeg-patches series must be updated when the C ABI break lands (Phase 4b.8);
  one-time migration cost.
- No external native package install path after Phase 4b.9; users outside Docker / k8s
  must build from source.
- kubebuilder / controller-runtime learning curve for contributors unfamiliar with
  Kubernetes Operator patterns.
- rclone FUSE mount adds a kernel-level FUSE dependency inside the node container; must
  be validated against distroless image constraints.

**Neutral / follow-ups:**

- The in-flight Phase 4a agents (vmafx-server, vmafx-mcp, vmafx-tune, vmafx-sys Rust
  bindings, C++23 internals) finish before Phase 4b sweeps start. Each completed agent
  output becomes an input dependency for the corresponding Phase 4b phase.
- Each Phase 4b.N sweep ships its own child ADR, research digest (where applicable),
  changelog fragment, and `docs/rebase-notes.md` entry.
- Netflix pipeline function audit runs in parallel as a research-only agent; its output
  integrates into Phase 4b prioritization.
- Thin clients (vmafx-mcp, vmafx-tune) are rewired to the controller gRPC API in
  follow-up PRs after Phase 4b.1 lands.

## References

**Parent ADRs:**

- [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) — VMAFX rebrand and
  aggressive modernization umbrella (Phase 1 + 2).
- [ADR-0701](0701-vmafx-cloud-native-redesign.md) — VMAFX cloud-native redesign
  (Phase 3: server-mode, Dockerfile, Helm chart, observability).
- [ADR-0702](0702-vmafx-phase4-language-modernization.md) — VMAFX Phase 4 multi-language
  modernization foundation (Phase 4a: Go workspace, Rust workspace, C++23 policy).
- [ADR-0699](0699-vmafx-helm-chart-k8s.md) — Helm chart + k8s manifests (Phase 3).
- [ADR-0706](0706-vmafx-rust-sys-bindings.md) — Rust `vmafx-sys` FFI crate (Phase 4a).

**Memory files consulted:**

- `project_vmafx_phase4b_distributed_platform.md` — locked Phase 4b decisions, popup
  answers verbatim, in-flight agent status.
- `project_vmafx_k8s_cloud_native.md` — Phase 3 cloud-native redesign decisions.
- `project_vmafx_phase4_language_modernization.md` — Phase 4a language modernization.
- `project_vmafx_rebrand_plan.md` — Phase 1+2 rebrand plan.

**Verbatim user popup answers (req):**

- `req` — "of course this has to be fully connected to a ffmpeg worker as well (latest
  of course)... and I think it was (thanks lawrence) that we should use rclone for using
  files without copying to disk/ram first? and if possible do ebpf optimizations..."
  (architecture popup, 2026-05-28)
- `req` — "Inside vmafx-node via Go ONNX Runtime (Recommended)" (AI inference popup,
  2026-05-28)
- `req` — "ml training in python only is wrong as well -> we want sidecar training while
  encoding etc.... (look at the 1000 things our software can do)" (training popup,
  2026-05-28)
- `req` — "option one but: we also are still missing (i think there was an audit file
  somewhere) the rests of the netflix pipeline functions" (in-flight agents popup,
  2026-05-28)
- `req` — "we rewrite and we update the patches and then we are fine? because I don't
  care about what others do with my project or not" (C ABI break popup, 2026-05-28)
- `req` — "now that I concentrate on docker images/k8s, do we still need to build native
  things?... the only thing we still need is the patches for ffmpeg?" (native builds
  popup, 2026-05-28)
