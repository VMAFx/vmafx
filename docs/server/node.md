<!-- markdownlint-disable MD013 MD060 -->
# vmafx-node — Worker Binary

`vmafx-node` is the data-plane worker in the VMAFX distributed platform
(Phase 4b, ADR-0709).  It connects to `vmafx-controller`, pulls scoring jobs,
executes them against `libvmaf`, and reports results back.

## Quick start (local)

```bash
# 1. Start a controller (Phase 4b.1).
./vmafx-controller &

# 2. Start a node (listens on :50052 by default).
export VMAFX_LOG_LEVEL=debug
./vmafx-node
```

The node auto-detects the available GPU backend.  Full controller-to-node
registration (pull-based job dispatch) is tracked in ADR-0713 Stage 2 —
see the planned env vars table below.

## Configuration (12-factor env vars)

| Variable | Default | Description |
|---|---|---|
| `VMAFX_FFMPEG_BIN` | `ffmpeg` (PATH) | Path to the `ffmpeg` binary.  The node Docker image sets this to `/usr/local/bin/ffmpeg` (ADR-0717). |
| `VMAFX_LOG_LEVEL` | `info` | Structured log level: `debug`, `info`, `warn`, `error` |
| `VMAFX_NODE_ADDR` | `:50052` | gRPC listen address for the node's worker service. |

See also the [full environment variable reference](../usage/env-vars.md) for the complete table.

### Planned env vars (ADR-0713 spec, not yet implemented)

The following variables appeared in the original Phase 4b.1 design (ADR-0713)
but are not currently read by the node binary.  They are reserved for a future
implementation pass.

| Variable | Planned default | Planned behaviour |
|---|---|---|
| `VMAFX_CONTROLLER_ADDR` | _(required)_ | Controller gRPC address for job pull (node-to-controller registration flow) |
| `VMAFX_NODE_ID` | hostname | Human-readable node name sent in `RegisterNode` |
| `VMAFX_BACKEND` | auto-detected | Force a specific backend: `cpu`, `cuda`, `sycl`, `hip`, `metal` |
| `VMAFX_GPU_DEVICE` | `0` | GPU device index (for multi-GPU hosts) |

## GPU auto-detection

On startup the node runs the following probes (in order):

1. `nvidia-smi -L` — NVIDIA GPU list.
2. `rocm-smi --showid` — AMD GPU list.
3. `clinfo` — Intel GPU via OpenCL platform name.
4. `system_profiler SPDisplaysDataType` — Apple Metal (macOS only).
5. CPU fallback — always succeeds.

The detected vendor maps to a backend preference:

| Vendor | Preferred backends |
|---|---|
| NVIDIA | `cuda`, `vulkan`, `cpu` |
| AMD    | `hip`, `vulkan`, `cpu` |
| Intel  | `sycl`, `vulkan`, `cpu` |
| Apple  | `metal`, `cpu` |
| CPU    | `cpu` |

Set `VMAFX_BACKEND` to override the auto-selected backend.

## Supported job types (Stage 1)

| Job type | Status | Description |
|---|---|---|
| `SCORING` | Supported | Encode (optional) → `libvmaf.Score` → return result |
| `AI`      | Unsupported (Stage 2) | ONNX inference; blocked on input transport in proto |
| `COMPARE` | Unsupported (Stage 2) | Multi-encode + score comparison ladder |

## Prometheus metrics

The node exposes metrics on `:9090/metrics`:

| Metric | Type | Description |
|---|---|---|
| `vmafx_node_jobs_total{outcome}` | Counter | Total jobs by outcome (`success`, `failure`) |
| `vmafx_node_job_duration_seconds` | Histogram | Job wall-clock duration |
| `vmafx_node_heartbeat_errors_total` | Counter | Heartbeat RPC failures |

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

The node Deployment sets `VMAFX_CONTROLLER_ADDR` automatically from the
in-cluster controller Service name (`<release>-controller:8080`).

## Container images

| Variant | `GPU_RUNTIME` ARG | Base |
|---|---|---|
| `vmafx-node:cpu` | `cpu` (default) | ubuntu:26.04 |
| `vmafx-node:cuda12` | `cuda12` | ubuntu:26.04 + CUDA 12 runtime |
| `vmafx-node:rocm6` | `rocm6` | ubuntu:26.04 + ROCm 6 runtime |
| `vmafx-node:sycl-oneapi2026` | `sycl-oneapi2026` | ubuntu:26.04 + Intel oneAPI 2026 |

Build example:

```bash
docker build -f docker/Dockerfile.node \
  --build-arg GPU_RUNTIME=cuda12 \
  -t vmafx-node:cuda12 .
```

## Graceful shutdown

On `SIGTERM` the node:

1. Cancels the work loop and heartbeat goroutine.
2. Finishes the current job (up to 30 s).
3. Shuts down the Prometheus HTTP server.
4. Exits with code 0.

If the current job does not finish within 30 s the node logs a warning and
forces exit.

## Development

```bash
# Run unit tests.
go test ./pkg/gpu/ ./pkg/ai/ ./cmd/vmafx-node/ -v

# Run with a local controller.
VMAFX_CONTROLLER_ADDR=localhost:8080 go run ./cmd/vmafx-node/

# Integration lifecycle test (requires a live controller).
VMAFX_INTEGRATION=1 go test ./cmd/vmafx-node/ -run TestNodeLifecycle -v
```

See also: [ADR-0713](../adr/0713-vmafx-node-impl.md),
[ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md),
[ADR-0711](../adr/0711-vmafx-controller-impl.md).
