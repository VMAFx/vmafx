# AGENTS.md — cmd/vmafx-node

Go gRPC scoring worker node with async online-training sidecar feedback
channel. Wired on golusoris fx framework (ADR-1119, Phase-1 PR-3). See
[ADR-0713](../../docs/adr/0713-vmafx-node-impl.md) (worker binary),
[ADR-0781](../../docs/adr/0781-sidecar-sgd-ema-online-trainer.md) (feedback
channel), and
[ADR-0933](../../docs/adr/0933-grpc-streaming-multi-frame-scoring.md)
(ScoreStream).

Node serves single gRPC service: `VmafxScoring` (`Score`, `ScoreStream`,
`Health`); node = **gRPC-only** (no HTTP server). eBPF rclone-bypass loader
under `bpf/` = privileged, opt-in side path unrelated to golusoris, NOT wired
into fx graph.

## Rebase-sensitive invariants

1. **fx owns signals and shutdown** (`main.go`, ADR-1119): node =
   `fx.New(...).Run()` application. fx installs SIGINT/SIGTERM handler, drives
   graceful shutdown through lifecycle. Do NOT reintroduce
   `signal.NotifyContext`, bespoke `server.Serve` loop, or manual
   `observability.InitOTel` / `slog.NewJSONHandler` (removed). Log
   level/format from golusoris `log` module (config keys `log.level` /
   `log.format`, honouring `VMAFX_` prefix); gRPC server,
   OTel/logging/recovery interceptors, listener lifecycle from golusoris
   `grpc.Module`.

2. **R-node — lifecycle stop order** (`main.go` + `providers.go`, ADR-1119):
   cgo `*libvmaf.Scorer` and per-call `StreamScorer` C contexts must be
   released only after in-flight `Score` / `ScoreStream` RPCs drain; sidecar
   `FeedbackClient` drainer must stop in between. Composition root forces
   **construction order**: scorer first
   (`fx.Invoke(func(_ *libvmaf.Scorer) {})`), then lifecycle domain objects
   (`fx.Invoke(func(_ *FeedbackClient, _ *Executor) {})`), then golusoris
   `*grpc.Server` registration + lazy-provider guard. fx runs OnStop hooks
   in reverse construction order: gRPC `GracefulStop` -> FeedbackClient drainer
   stop -> scorer `Close()`. `TestStopOrderNode` (`app_test.go`) pins REAL hook
   order via `fxevent.Logger`. Do NOT reorder those invokes, flip arg order, or
   move scorer/feedback Close hooks to `*grpc.Server`-gated invoke -> inverts
   construction order, closes scorer / stops drainer while RPCs still in
   flight.

3. **Lazy-provider gRPC listener guard** (`main.go`, ADR-1119): fx providers
   lazy — `grpc.Module` OnStart listener binds only if something consumes
   `*grpc.Server`. Standalone `fx.Invoke(func(_ *grpc.Server) {})` =
   load-bearing; without it node serves nothing.
   `fx.Invoke(func(_ *FeedbackClient, _ *Executor) {})` guard starts feedback
   drainer, makes executor exist (nothing in scoring path consumes either).
   `TestAppStartsAndBinds` dials bound address, calls `Health` to prove
   listener came up, service wired.

4. **FeedbackClient drainer lifetime** (`online_feedback.go`, ADR-1119):
   `NewFeedbackClient(log)` constructs client WITHOUT spawning goroutine and
   WITHOUT caller context. `Start()` launches drainer (bound to internal,
   `Close`-owned context); `Close()` cancels it, awaits goroutine. Both
   idempotent (`sync.Once` + `atomic.Bool`); `Close` correct whether or not
   `Start` ran. Wired to fx OnStart / OnStop in `provideFeedbackClient`. Do NOT
   revert to ctx-bound constructor spawning at construction time -> leaked
   goroutine past `Close`, bound drainer to caller lifetime fx does not own.
   Newline-delimited JSON wire protocol, bounded ring-buffer drop semantics
   unchanged. Admission-aware trainer failures use `ok: true`,
   `retry_queued: true`, and `training_error`: the sidecar retained the sample,
   so the Go client counts it delivered and logs the deferred training error
   without resubmitting. A capacity-deferred sample uses `ok: false` and
   `retryable: true`; the client requeues it, returns to the reconnect loop, and
   does not increment `delivered`. Non-retryable `ok: false` remains terminal.
   Keep `feedbackAck` synchronized with the Python response.

5. **Encoder probe is NON-FATAL and runs in OnStart** (`providers.go`,
   ADR-0717): `provideEncoderInventory` returns shared `*probe.Inventory`
   (empty at construction), populates it in OnStart hook bounded by
   `probeTimeout` (~30 s). Probe failure logs WARN, leaves inventory empty;
   node still serves. Inventory pointer shared with scoring handler, filled
   before gRPC listener binds (Inventory constructed before `*grpc.Server` ->
   its OnStart hook runs first) -> no concurrent read of slices it fills. Keep
   probe NON-FATAL.

6. **Scorer is nil-tolerant** (`providers.go`): missing vmaf binary =
   NON-FATAL. `provideScorer` returns `nil`; node serves `Health` (k8s
   liveness), scoring RPCs return `codes.FailedPrecondition`.
   `newScoringHandler` / `mountNodeHealth` nil-guards depend on this.

7. **`UnimplementedVmafxScoringServer` embedding** (`scoring_handler.go`):
   `scoringHandler` struct embeds `vmafxv1.UnimplementedVmafxScoringServer`
   so future proto additions do not break build. Do not remove embed.

8. **`ScoreStream` framing + after-EOF scores** (`scoring_handler.go`,
   ADR-0933): first `ScoreStreamRequest` MUST set `config` oneof; every
   subsequent request MUST set `frame_pair`. Per-frame scores emitted only
   after client half-closes (temporal VMAF features finalise at flush).
   Contract identical to vmafx-server handler: do not "stream scores as frames
   arrive".

9. **golusoris config sub-keys** (`main.go` / `providers.go`, ADR-1119): node
   reads listen address from `grpc.listen` (`VMAFX_GRPC_LISTEN`, replaces
   removed `VMAFX_NODE_ADDR`; historical `:50052` default preserved); domain
   settings from `vmaf.binary` / `model.dir` / `ffmpeg.bin` / `backend` /
   `sidecar.socket`. `fx.Replace(config.Options{
   EnvPrefix: "VMAFX_", ...})` line = load-bearing: without it graph reads
   framework default `APP_` prefix, ignores every `VMAFX_*` var.
   `fx.Decorate(withNodeGRPCDefault)` line = load-bearing: changes only empty
   raw `grpc.listen` to `:50052`; operator explicitly selecting `:9090` via
   file or env remains intact.

10. **go.mod golusoris pin** (`go.mod`, ADR-1119): `golusoris/golusoris` pinned
    at `v0.7.0` (module-wide). Do not change pin or edit
    `internal/app/bootstrap` from this package (both shared across all
    binaries).

11. **`--version` exits before fx startup** (`main.go`, ADR-1129): release
    images inject `pkg/version.version` via Go ldflags; container smoke
    executes `vmafx-node --version`. Keep exact early exit ahead of
    `fx.New(...).Run()` so version verification never starts long-running
    gRPC listener or requires scoring assets.

12. **OTel init is `bootstrap.Base`, spans are `grpcmod` + `executor.go`**
    (ADR-0782 / ADR-1119): no HTTP server -> carries no
    `bootstrap.HTTPTracing`. gRPC server spans from `grpcmod.Module` `otelgrpc`
    handler; job spans (`vmafx.scoring`, `vmafx.frame.extraction`,
    `vmafx.onnx.inference`) from `executor.go` via `observability.StartSpan`.
    `app_test.go::TestOTelWiredThroughBootstrap` locks no-op default and
    `vmafx-node` / `pkg/version` identity.
