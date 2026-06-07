# vmafx-server Go gRPC + HTTP service (ADR-0703)

Adds `cmd/vmafx-server` — a production Go binary that serves VMAF scoring over
gRPC (`VmafxScoring` service, `proto/vmafx.proto`) and HTTP/JSON with full parity
with the Python server from PR #1583.

- **gRPC** (default port 50051): `Score(ScoreRequest)` returns aggregate VMAF score
  and per-feature map; `Health()` liveness RPC.
- **HTTP** (default port 8080): `/healthz`, `/readyz`, `/metrics` (Prometheus),
  `POST /v1/score`.
- **libvmaf cgo wrapper** (`pkg/libvmaf/`): `New / Score / Close` typed Go API backed
  by the `vmaf` CLI binary.
- **Observability** (`pkg/observability/`): `log/slog` JSON logger, Prometheus registry
  with `vmafx_server_*` counters + latency histogram, Go runtime + process collectors,
  30 s SIGTERM graceful shutdown.
- **12-factor config**: `VMAFX_PORT`, `VMAFX_GRPC_PORT`, `VMAFX_LOG_LEVEL`,
  `VMAFX_VMAF_BINARY`, `VMAFX_MODEL_DIR`.
- **Multi-stage Dockerfile** (`Dockerfile.go-server`): `golang:1.23-bookworm` builder
  + `gcr.io/distroless/cc-debian12` runtime; EXPOSE 8080 + 50051.
- Python implementation retained as a compat layer pending Stage-3 cleanup PR.
