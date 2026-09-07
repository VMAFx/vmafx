# AGENTS.md — cmd/vmafx-node

Go gRPC scoring worker node with an async online-training sidecar feedback
channel. Wired on the golusoris fx framework (ADR-1119, Phase-1 PR-3). See
[ADR-0713](../../docs/adr/0713-vmafx-node-impl.md) (worker binary),
[ADR-0781](../../docs/adr/0781-sidecar-sgd-ema-online-trainer.md) (feedback
channel), and
[ADR-0933](../../docs/adr/0933-grpc-streaming-multi-frame-scoring.md)
(ScoreStream).

The node serves a single gRPC service — `VmafxScoring` (`Score`, `ScoreStream`,
`Health`) — and is **gRPC-only** (no HTTP server). The eBPF rclone-bypass loader
under `bpf/` is a privileged, opt-in side path unrelated to golusoris and is NOT
wired into the fx graph.

## Rebase-sensitive invariants

1. **fx owns signals and shutdown** (`main.go`, ADR-1119): the node is an
   `fx.New(...).Run()` application. fx installs the SIGINT/SIGTERM handler and
   drives graceful shutdown through the lifecycle. Do NOT reintroduce
   `signal.NotifyContext`, a bespoke `server.Serve` loop, or a manual
   `observability.InitOTel` / `slog.NewJSONHandler` — those were removed. Log
   level/format come from the golusoris `log` module (config keys
   `log.level` / `log.format`, honouring the `VMAFX_` prefix); the gRPC server,
   its OTel/logging/recovery interceptors, and its listener lifecycle come from
   golusoris `grpc.Module`.

2. **R-node — lifecycle stop order** (`main.go` + `providers.go`, ADR-1119): the
   cgo `*libvmaf.Scorer` and the per-call `StreamScorer` C contexts must be
   released only after in-flight `Score` / `ScoreStream` RPCs drain, and the
   sidecar `FeedbackClient` drainer must stop in between. The composition root
   guarantees this by forcing **construction order**: scorer first
   (`fx.Invoke(func(_ *libvmaf.Scorer) {})`), then the lifecycle-bearing domain
   objects (`fx.Invoke(func(_ *FeedbackClient, _ *Executor) {})`), then the
   golusoris `*grpc.Server` registration + the lazy-provider guard. Because fx
   runs OnStop hooks in reverse of construction order, the firing sequence is:
   gRPC `GracefulStop` → FeedbackClient drainer stop → scorer `Close()`.
   `TestStopOrderNode` (`app_test.go`) pins this against the REAL hook order via
   an `fxevent.Logger`. Do NOT reorder those invokes, flip arg order, or move
   the scorer/feedback Close hooks to a `*grpc.Server`-gated invoke — any of
   those inverts construction order and closes the scorer / stops the drainer
   while RPCs are still in flight.

3. **Lazy-provider gRPC listener guard** (`main.go`, ADR-1119): fx providers are
   lazy — `grpc.Module`'s OnStart listener only binds if something consumes
   `*grpc.Server`. The standalone `fx.Invoke(func(_ *grpc.Server) {})` is
   load-bearing; without it the node serves nothing. Likewise the
   `fx.Invoke(func(_ *FeedbackClient, _ *Executor) {})` guard is what makes the
   feedback drainer start and the executor exist (nothing in the scoring path
   consumes either). `TestAppStartsAndBinds` dials the bound address and calls
   `Health` to prove the listener came up and the service is wired.

4. **FeedbackClient drainer lifetime** (`online_feedback.go`, ADR-1119):
   `NewFeedbackClient(log)` constructs the client WITHOUT spawning a goroutine
   and WITHOUT taking a caller context. `Start()` launches the drainer (bound to
   an internal, `Close`-owned context); `Close()` cancels it and awaits the
   goroutine. Both are idempotent (`sync.Once` + an `atomic.Bool`), and `Close`
   is correct whether or not `Start` ran. They are wired to fx OnStart / OnStop
   in `provideFeedbackClient`. Do NOT revert to the ctx-bound constructor that
   spawned at construction time — it leaked a goroutine past `Close` and bound
   the drainer to a caller lifetime fx does not own. The newline-delimited JSON
   wire protocol and the bounded ring-buffer drop semantics are unchanged.

5. **Encoder probe is NON-FATAL and runs in OnStart** (`providers.go`,
   ADR-0717): `provideEncoderInventory` returns a shared `*probe.Inventory`
   (empty at construction) and populates it in an OnStart hook bounded by
   `probeTimeout` (~30 s). A probe failure logs a WARN and leaves the inventory
   empty — the node still serves. The Inventory pointer is shared with the
   scoring handler; it is filled before the gRPC listener binds (the Inventory
   is constructed before `*grpc.Server`, so its OnStart hook runs first), so
   there is no concurrent read of the slices it fills. Keep the probe NON-FATAL.

6. **Scorer is nil-tolerant** (`providers.go`): a missing vmaf binary is
   NON-FATAL — `provideScorer` returns `nil` and the node serves `Health`
   (k8s liveness) while the scoring RPCs return `codes.FailedPrecondition`.
   The `newScoringHandler` / `mountNodeHealth` nil-guards depend on this.

7. **`UnimplementedVmafxScoringServer` embedding** (`scoring_handler.go`): the
   `scoringHandler` struct embeds `vmafxv1.UnimplementedVmafxScoringServer` so
   future proto additions don't break the build. Do not remove the embed.

8. **`ScoreStream` framing + after-EOF scores** (`scoring_handler.go`,
   ADR-0933): the first `ScoreStreamRequest` MUST set the `config` oneof; every
   subsequent request MUST set `frame_pair`; per-frame scores are emitted only
   after the client half-closes (temporal VMAF features finalise at flush). This
   contract is identical to the vmafx-server handler — do not "stream scores as
   frames arrive".

9. **golusoris config sub-keys** (`main.go` / `providers.go`, ADR-1119): the
   node reads its listen address from `grpc.listen` (`VMAFX_GRPC_LISTEN`,
   replacing the removed `VMAFX_NODE_ADDR`; historical `:50052` default
   preserved) and its domain settings from `vmaf.binary` / `model.dir` /
   `ffmpeg.bin` / `backend` / `sidecar.socket`. The `fx.Replace(config.Options{
   EnvPrefix: "VMAFX_", ...})` line is load-bearing — without it the graph reads
   the framework default `APP_` prefix and ignores every `VMAFX_*` var. The
   `fx.Decorate(withNodeGRPCDefault)` line is also load-bearing: it changes only
   an empty raw `grpc.listen` to `:50052`, so an operator explicitly selecting
   the framework's `:9090` through either a file or environment remains intact.

10. **go.mod golusoris pin** (`go.mod`, ADR-1119): `golusoris/golusoris` is
    pinned at `v0.7.0` (module-wide). Do not change the pin or edit
    `internal/app/bootstrap` from this package — both are shared across all
    binaries.

11. **`--version` exits before fx startup** (`main.go`, ADR-1129): release
    images inject `pkg/version.version` through Go ldflags and the container
    smoke executes `vmafx-node --version`. Keep this exact early exit ahead of
    `fx.New(...).Run()` so version verification never starts the long-running
    gRPC listener or requires scoring assets.

12. **OTel init is `bootstrap.Base`, spans are `grpcmod` + `executor.go`**
    (ADR-0782 / ADR-1119): the node has no HTTP server, so it carries no
    `bootstrap.HTTPTracing`; its gRPC server spans come from `grpcmod.Module`'s
    `otelgrpc` handler and its job spans (`vmafx.scoring`,
    `vmafx.frame.extraction`, `vmafx.onnx.inference`) from `executor.go` via
    `observability.StartSpan`. `app_test.go::TestOTelWiredThroughBootstrap`
    locks the no-op default and the `vmafx-node` / `pkg/version` identity.
