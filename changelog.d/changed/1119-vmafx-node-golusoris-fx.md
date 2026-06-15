- **`vmafx-node` migrated onto the golusoris fx framework** (ADR-1119,
  Phase-1 PR-3). The hand-rolled composition root (`signal.NotifyContext`,
  the bespoke `server.New` / `server.Serve` gRPC lifecycle, the manual
  `observability.InitOTel` init, and the `slog.NewJSONHandler` + `logLevel()`
  logger) is replaced by an `fx.New(...).Run()` over golusoris modules:
  `golusoris.Core` (config + structured slog logging), `otel.Module`, and the
  golusoris `grpc.Module` (OTel + logging + recovery interceptors baked in).
  The node stays gRPC-only and keeps serving the `VmafxScoring` service
  (`Score`, `ScoreStream`, `Health`); the libvmaf cgo scorer, the executor, the
  encoder probe, the online-training sidecar feedback channel, and the eBPF
  rclone-bypass loader are unchanged in behaviour.
- **Lifecycle hardening.** The startup encoder probe and the sidecar
  `FeedbackClient` drainer now run under fx lifecycle hooks: the probe runs in
  an `OnStart` hook (~30 s timeout, NON-FATAL on failure), and the feedback
  drainer is launched in `OnStart` and `Close()`d + awaited in `OnStop`
  (`NewFeedbackClient` no longer spawns a goroutine at construction — it gains a
  `Start()` method bound to an internal, leak-free context). Shutdown order is
  pinned: gRPC `GracefulStop` drains in-flight RPCs → feedback drainer stops →
  libvmaf scorer closes.
- **Breaking — node env-var contract.** The gRPC listen address moves from the
  bare-address `VMAFX_NODE_ADDR` to golusoris' `VMAFX_GRPC_LISTEN`
  (koanf key `grpc.listen`); the historical `:50052` default is preserved.
  Domain settings are read from the golusoris config tree
  (`VMAFX_VMAF_BINARY` → `vmaf.binary`, `VMAFX_MODEL_DIR` → `model.dir`,
  `VMAFX_FFMPEG_BIN` → `ffmpeg.bin`, `VMAFX_BACKEND` → `backend`,
  `VMAFX_SIDECAR_SOCKET` → `sidecar.socket`). `VMAFX_LOG_LEVEL` is now read by
  the golusoris `log` module (key `log.level`, honours the `VMAFX_` prefix).
  Update deployment manifests; see `docs/usage/env-vars.md`.
