- Go context-propagation sweep: callers that previously dropped a
  `context.Context` at the subprocess / SQLite boundary now forward it via
  `exec.CommandContext` / `(*sql.DB).ExecContext`. Affected sites:
  - `pkg/libvmaf/scorer` — added `ScoreContext(ctx, …)` that wraps the
    `vmaf` CLI invocation in `exec.CommandContext`. `Score(…)` is kept as a
    backwards-compatible wrapper around `ScoreContext(context.Background(), …)`
    and marked deprecated.
  - `cmd/vmafx-controller/{grpc_server,http_server}.go` and
    `cmd/vmafx-server/{grpc_server,http_server}.go` — `/v1/score` and the
    gRPC `Score` RPC now forward `r.Context()` / the gRPC context into
    `ScoreContext`, so a client disconnect or graceful-shutdown signal aborts
    the underlying `vmaf` subprocess instead of leaving a zombie.
  - `cmd/vmafx-node/executor.go` — the node executor forwards the job's
    `ctx` into `ScoreContext` so a controller-side cancellation reaches the
    running scorer.
  - `cmd/vmafx-controller/queue/queue.go` — `Submit`, `PullWork`,
    `ReportResult`, and `Cancel` now use `db.ExecContext` with the caller's
    `ctx` (previously they took `_ context.Context` and dropped it).
  - `cmd/vmafx-node/probe/probe.go` — `EncoderInventory(ctx, ffmpegBin)`
    now accepts and forwards a context; `cmd/vmafx-node/main.go` binds the
    startup probe to a 30 s timeout so a hung `ffmpeg` cannot stall node
    boot.
  - `cmd/vmafx-mcp/impl.go` — the `vmaf_score`, `probe_backend`,
    `eval_model_on_split`, `compare_models`, and `describe_worst_frames`
    handlers now plumb the MCP tool-call context through `runVmafScore` /
    `delegateToPythonEval` and into `exec.CommandContext`.
