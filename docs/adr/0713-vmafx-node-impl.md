<!-- markdownlint-disable MD013 MD060 -->
# ADR-0713: vmafx-node Go Worker Binary

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `go`, `node`, `grpc`, `libvmaf`, `cgo`, `onnx`, `ffmpeg`, `k8s`, `phase4b`, `fork-local`

## Context

Phase 4b (ADR-0709) defined a controller/node split for the distributed VMAFX
platform.  ADR-0711 shipped the `vmafx-controller` binary (job queue, node
registry, scheduler, gRPC `VmafxController` service).  The worker side — the
`vmafx-node` binary that actually runs libvmaf scoring, ffmpeg encodes, and AI
inference — was deferred to Phase 4b.2.

The node binary must:

- Connect to the controller at startup, announce GPU capabilities, and pull jobs.
- Execute SCORING jobs by calling libvmaf directly via cgo (not subprocess) for
  low latency and to avoid the subprocess overhead on GPU backends.
- Execute AI jobs by invoking the ORT runner (Stage 1 subprocess; Stage 2 will
  use `github.com/yalue/onnxruntime_go` directly).
- Report results back to the controller via `ReportResult` RPC.
- Expose Prometheus metrics on `:9090/metrics`.
- Shut down gracefully within 30 s on SIGTERM.
- Be deployable as a Kubernetes Deployment with GPU device-plugin resource
  requests (NVIDIA/AMD/Intel).

## Decision

The Phase 4b.2 implementation delivers the following in a single PR:

### Binary: `cmd/vmafx-node/`

A single static Go binary built with `CGO_ENABLED=1` against `libvmaf.so`.
The lifecycle is:

1. Load 12-factor config from env vars (`VMAFX_CONTROLLER_ADDR`, `VMAFX_NODE_ID`,
   `VMAFX_BACKEND`, `VMAFX_GPU_DEVICE`, `VMAFX_LOG_LEVEL`, `VMAFX_MODEL_DIR`).
2. Probe GPU hardware via `pkg/gpu.Detect()`.
3. Probe ffmpeg codec availability via `pkg/encoder.AvailableHardwareEncoders()`.
4. Connect to controller; call `RegisterNode`.
5. Start heartbeat goroutine (10 s interval).
6. Start main work loop: `PullWork` → `Execute` → `ReportResult`.
7. On SIGTERM: cancel context, drain the current job (30 s timeout), exit.

### Package: `pkg/gpu/`

GPU vendor detection using subprocess probes: `nvidia-smi -L` (NVIDIA),
`rocm-smi --showid` (AMD), `clinfo` with Intel platform filter (Intel), and
`system_profiler SPDisplaysDataType` (Apple Metal on macOS).  Returns a
`Capability` struct with vendor, device count, device name, compute capability
(NVIDIA only), and ordered backends list.

### Package: `pkg/ai/`

Registry for ONNX model files under `VMAFX_MODEL_DIR`.  Stage 1 uses a
subprocess (`vmafx-ort-runner`) to run ORT sessions; this avoids CGO coupling
on libtensorrt at the Go layer.  Stage 2 will promote `InferDirect` (stubbed)
to the main path using `github.com/yalue/onnxruntime_go`.

### Package: `pkg/encoder/` (extended)

Added `discover.go` (codec probing via `ffmpeg -encoders`, cached) and
`hardware.go` (NVENC, QSV, AMF, SVT-AV1, libaom-av1 encoder implementations).
QSV injects the VA-API device init chain (ADR-0601 pattern) via `ExtraArgs`.

### Generated stubs: `gen/go/controller/`

`controller.pb.go` and `controller_grpc.pb.go` — manually-maintained mirrors
of what `protoc-gen-go` / `protoc-gen-go-grpc` would generate from
`cmd/vmafx-controller/proto/controller.proto` (ADR-0711).  Included in-tree so
the CI matrix does not require `buf`/`protoc` at build time.

### Container: `docker/Dockerfile.node`

Multi-stage Ubuntu 24.04 build with `ARG GPU_RUNTIME=cpu|cuda12|rocm6|sycl-oneapi2026`.
The builder stage compiles libvmaf (CPU) and the Go binary.  The runtime stage
installs ffmpeg, copies `libvmaf.so`, and ships the stripped binary.  GPU SDK
layers are conditional on `ARG GPU_RUNTIME`.

### Helm: `deploy/helm/vmafx/templates/node.yaml`

Worker pool `Deployment` + `Service` (metrics port 9090) gated on
`.Values.node.enabled`.  Injects `VMAFX_CONTROLLER_ADDR` from the in-cluster
controller Service name.  GPU resource requests derive from `.Values.gpu.vendor`
via the same `vmafx.gpuResourceKey` helper used by the controller deployment.
Node-specific `nodeSelector` and `tolerations` override global values.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Direct ORT CGO (Stage 1) | One fewer subprocess | Requires libtensorrt or libonnxruntime at Go build time; container layers grow ~2 GB; CI matrix needs ORT pre-installed | Deferred to Stage 2 where the container image pins ORT 1.18+ |
| Per-vendor Dockerfile (no ARG) | Simpler per-variant build | Four separate files with near-identical content; drift risk | ARG GPU_RUNTIME with conditional RUN blocks keeps duplication minimal |
| Sidecar container for encoding | Decouples ffmpeg lifecycle | Adds pod complexity, inter-container networking overhead | In-process subprocess is simpler for Stage 1 |
| Python worker | Reuses existing quality_runner harness | No direct cgo libvmaf access; subprocess per-job startup cost; no static binary | Entire Phase 4 commitment is to Go for the data plane |

## Consequences

- **Positive**: The fork now has a complete controller/node loop runnable in
  a local Kind cluster; end-to-end VMAF scoring over gRPC is available.
- **Positive**: Hardware codec availability is probed at startup — no hard
  failure when the GPU driver is absent on a CPU-only node.
- **Negative**: Stage 1 AI jobs are not fully wired (no input transport in the
  proto); AI inference will return an error until Stage 2 adds the feature tensor
  transport field to `ScoringParams`.
- **Neutral**: The `pkg/ai/InferDirect` stub anchors the Stage 2 refactor;
  `github.com/yalue/onnxruntime_go` is not yet a go.mod dependency (it requires
  a pre-built ORT shared library at link time).

## References

- ADR-0709: VMAFX Phase 4b distributed platform.
- ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
- ADR-0601: vmafx-tune QSV/AMF hw-init + probe fix (QSV init chain pattern).
- Source: per user direction (Phase 4b.2 task brief, 2026-05-28).
