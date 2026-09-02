<!-- markdownlint-disable MD013 MD060 -->
# vmafx-node — Worker Binary

`vmafx-node` is the data-plane scoring worker in the VMAFX distributed
platform (Phase 4b, ADR-0709). It serves the `VmafxScoring` gRPC API and
executes score requests against `libvmaf`.

## Quick start (local)

```bash
# Start a node (listens on :50052 by default).
export VMAFX_LOG_LEVEL=debug
./vmafx-node
```

The node defaults to CPU scoring. Set `VMAFX_BACKEND` explicitly, or use a
GPU-specific container target, to select another compiled backend.

## gRPC service the node serves

The node hosts the **`VmafxScoring`** service (the same contract as
`vmafx-server`) on `VMAFX_GRPC_LISTEN`, so any gRPC client can dispatch scoring
directly to a node. See
[ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md).

| RPC | Shape | Notes |
|---|---|---|
| `Score` | unary | File-path reference/distorted pair → pooled VMAF + features. |
| `ScoreStream` | bidirectional stream | In-memory per-frame scoring (ADR-0933). One `StreamConfig`, then `FramePair` messages, then EOF; the node returns one `FrameScore` per frame plus a terminal `AggregateScore`. See [grpc-streaming.md](../architecture/grpc-streaming.md). |
| `Health` | unary | Liveness; answers even when no scorer is configured. |

The scoring engine is the shared cgo `pkg/libvmaf`. The node resolves models
from `VMAFX_MODEL_DIR`; if no `vmaf` binary / model dir is available the node
still serves `Health` and returns `codes.FailedPrecondition` from the scoring
RPCs. The controller-pull worker loop (`PullWork → Execute → ReportResult`,
ADR-0713) is a separate _client_ role and is orthogonal to this served surface.

Example:

```bash
grpcurl -plaintext localhost:50052 vmafx.v1.VmafxScoring/Health
# {"ok": true, "message": "ok"}
```

## Configuration (12-factor env vars)

| Variable | Default | Description |
|---|---|---|
| `VMAFX_GRPC_LISTEN` | `:50052` | gRPC listen address for the node's worker service. |
| `VMAFX_FFMPEG_BIN` | `ffmpeg` (PATH) | Path to the `ffmpeg` binary.  The node Docker image sets this to `/usr/local/bin/ffmpeg` (ADR-0717). |
| `VMAFX_VMAF_BINARY` | automatic lookup | Path to the `vmaf` CLI binary used for scoring. |
| `VMAFX_MODEL_DIR` | binary default | Directory containing VMAF model files. The node image sets `/usr/local/share/vmafx/model`. |
| `VMAFX_BACKEND` | `cpu` | Scoring backend label, such as `cpu`, `cuda`, `hip`, or `sycl`. |
| `VMAFX_SIDECAR_SOCKET` | `/tmp/vmafx-sidecar.sock` | Online-training sidecar Unix socket. |
| `VMAFX_LOG_LEVEL` | `info` | Structured log level: `debug`, `info`, `warn`, `error` |
| `VMAFX_LOG_FORMAT` | `auto` | Log handler: `auto`, `tint`, or `json`. |

See also the [full environment variable reference](../usage/env-vars.md) for the complete table.

## Backend selection

`VMAFX_BACKEND` defaults to `cpu`; the binary does not probe the host and
silently change backends. The published CPU image also defaults to `cpu`.
Locally built `node-cuda`, `node-rocm`, and `node-sycl` targets set `cuda`,
`hip`, and `sycl` respectively. A requested backend must be present in the
libvmaf build and usable on the host.

## Observability

The node emits structured logs and OpenTelemetry data through the shared
golusoris runtime. It is gRPC-only and does not expose a Prometheus HTTP
listener. The Helm chart probes the TCP listener on the configured gRPC port;
gRPC clients can use the `VmafxScoring/Health` RPC for an application-level
health check.

## Kubernetes deployment

The Helm chart (`deploy/helm/vmafx/`) ships a node worker pool Deployment gated
on `.Values.node.enabled`.  Enable it alongside the controller:

```yaml
# values.yaml
node:
  enabled: true
  replicaCount: 3
  nodeSelector:
    nvidia.com/gpu.present: "true"
  tolerations:
    - key: nvidia.com/gpu
      operator: Exists
      effect: NoSchedule

gpu:
  enabled: true
  vendor: nvidia
  count: 1
```

```bash
helm upgrade --install vmafx deploy/helm/vmafx/ -f values.yaml
```

## Container images

| Docker target | Published tag | Runtime |
|---|---|---|
| `node-cpu` | `vX.Y.Z` (amd64 + arm64) | distroless Debian 13 |
| `node-cuda` | not yet published | Debian 13 + CUDA 13.3.1 libraries |
| `node-rocm` | not yet published | Debian 13 + ROCm 7.2.4 libraries |
| `node-sycl` | not yet published | Debian 13 + oneAPI 2025.3.1 libraries |

The release workflow currently publishes only `node-cpu`. All targets use the
same native-architecture FFmpeg dependency collector, so arm64 stages resolve
`aarch64-linux-gnu` libraries rather than copying an amd64-only path.

Build example:

```bash
docker build -f docker/Dockerfile.node \
  --target node-cuda \
  -t vmafx-node:cuda13 .
```

## Graceful shutdown

On `SIGTERM` the node:

1. Gracefully stops the gRPC server and drains in-flight scoring RPCs.
2. Stops and joins the online-feedback sidecar drainer.
3. Closes the scorer after the gRPC drain completes.

## Development

```bash
# Run unit tests.
go test ./pkg/gpu/ ./pkg/ai/ ./cmd/vmafx-node/ -v

# Run the node locally on its default gRPC port.
go run ./cmd/vmafx-node/
```

See also: [ADR-0713](../adr/0713-vmafx-node-impl.md),
[ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md),
[ADR-0711](../adr/0711-vmafx-controller-impl.md).
