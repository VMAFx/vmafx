<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/vmafx-node

Go gRPC scoring worker node with async online-training sidecar feedback
channel. Wired on golusoris fx framework (ADR-1119, Phase-1 PR-3). See
[ADR-0713](../../docs/adr/0713-vmafx-node-impl.md) (worker binary),
[ADR-0781](../../docs/adr/0781-sidecar-sgd-ema-online-trainer.md) (feedback
channel), and
[ADR-0933](../../docs/adr/0933-grpc-streaming-multi-frame-scoring.md)
(ScoreStream).

Node serves single gRPC service: `VmafxScoring` (`Score`, `ScoreStream`,
`Health`) plus HTTP on `VMAFX_HTTP_ADDR` (default `:9090` = chart
`node.metricsPort`): `/metrics`, `/livez`, `/readyz`, `/startupz` only. With `VMAFX_CONTROLLER_ADDR`
set, node also = controller client (`controller_*.go`, ADR-1524): register,
heartbeat, pull, execute, report. eBPF descriptor tracker
under `bpf/` = privileged, opt-in (`VMAFX_EBPF_BYPASS`, `ebpf_linux.go`,
ADR-1539), fail closed.

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
   `retry_queued: true`, and `training_error`: sidecar retained sample,
   so Go client counts it delivered and logs deferred training error
   without resubmitting. capacity-deferred sample uses `ok: false` and
   `retryable: true`; client retains that in-flight sample locally across
   reconnects and retries it ahead of bounded queue without competing for    queue slot or incrementing `delivered`. Non-retryable `ok: false` remains terminal.
   Local JSON encoding failures are also terminal: increment `dropped`, keep    connection open, and continue with next queued sample so non-finite
   payload cannot poison drainer. Only transport failures and explicit
   retryable ACKs retain in-flight sample.
   Keep `feedbackAck` synchronized with Python response.

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
    (ADR-0782 / ADR-1119): HTTP listener carries `bootstrap.HTTPTracing`
    (probes + `/metrics` filtered). gRPC server spans from `grpcmod.Module` `otelgrpc`
    handler; job spans (`vmafx.scoring`, `vmafx.frame.extraction`,
    `vmafx.onnx.inference`) from `executor.go` via `observability.StartSpan`.
    `app_test.go::TestOTelWiredThroughBootstrap` locks no-op default and
    `vmafx-node` / `pkg/version` identity.

13. **Controller client contract** (`controller_*.go`, ADR-1524):
    `provideControllerClient` returns nil without `controller.addr` (logged,
    node standalone); returns error (startup refused) when addr set and
    scorer nil, `VMAFX_BACKEND` not in `backendVendors` (no `auto`), or
    `loadControllerConfig` rejects a key. Never turn these into a silent
    default. Lifecycle invoke `fx.Invoke(func(_ *FeedbackClient, _ *Executor,
    _ *controllerClient) {})` stays ahead of gRPC registration: stop order
    gRPC GracefulStop -> client drain -> feedback stop -> scorer Close
    (`TestStopOrderNodeWithController`). Client dials via golusoris
    `*grpcmod.ConnFactory` (otelgrpc, cmd invariant 3); every RPC own
    `context.WithTimeout(rpc_timeout)` (HISS-02). Node advertises ONE backend
    and `executeScoring` passes `jobBackend()` to `Scorer.ScoreOnBackend` as
    `--backend`; do not drop the flag or advertise backends the CLI does not
    run. Interrupted job at stop deadline -> reported failed, never left
    RUNNING. Controller-key underscore leaves live in `controllerConfigKeys`
    (CompoundKeys); `env_test.go` pins the set. E2E guard:
    `TestEndToEndControllerNodeJob` (real controller binary + real vmaf).
    TLS + bearer = `pkg/controllerclient` (`controllerConfig.Creds`, shared
    with the operator, ADR-1569); no node-local copy.
    Scoring roots (ADR-1577): `scopedSources` resolves both inputs under
    `job.GetScoringRoots()` (symlinks followed here) before `store.Prepare`;
    empty roots = job refused; the real path is scored. Never prepare or
    open an input first. Guard: `executor_scope_test.go`.
    Cancel (ADR-1567): each job runs under own
    `context.WithCancelCause(execCtx)`, cancel func kept in `jobs` by ID;
    heartbeat sends `running_job_ids` (sorted, <= slots <= 64); answer
    `cancel_job_ids` -> `cancel(errCancelledByController)` -> vmaf killed via
    `exec.CommandContext`; report = `cancelled by the controller: ...`. Never
    cancel IDs not in `jobs`; keep the cause check (shutdown wording differs).
    Guards: `controller_cancel_test.go`, `TestEndToEndCancelStopsTheNodesVmaf`.

14. **Storage wiring** (`executor_inputs.go`, `storage_config.go`,
    ADR-1526): `provideExecutor` builds the executor with
    `storage.Open` (never deprecated `storage.New`); unknown
    `storage.mode` / mount without FUSE -> startup error. `scoreJob` prepares
    both sources, defers both cleanups; two paths -> `ScoreOnBackend`, any
    http(s) input -> `ScoreReaders` (pipes, no disk). Do not download to a
    temp file and do not drop the stream-failure check: CLI scores a short
    distorted clip with exit 0. `NewExecutor` = `LocalStorage` (local paths
    only, remote URI refused). Guard: `TestEndToEndControllerNodeRcloneSources`
    (real rclone, both modes), `TestStorageModeRefusedAtStartup`.

15. **eBPF tracker wiring** (`ebpf_*.go`, `bpf/`, ADR-1539): invoke arg order
    `_ *FeedbackClient, _ *Executor, _ *ebpfBypass, _ *controllerClient`
    (tracker starts before pulls, stops after drain). Enabled + storage not
    mount / root outside prefix -> construction error; `bpf.Preflight`
    failure or `Start` error -> OnStart error (node exits). Never downgrade to
    a warning. Loader ctx = provider-owned `WithCancel`, not the fx start ctx
    (fx cancels that after start; drain loop would die). `bpf/` objects
    (ADR-1622): `rclonebypass_bpfel.o` NOT committed, git-ignored; generated by
    `scripts/dev/gen-node-bpf.sh` (`make node-bpf`) in Go CI, `Dockerfile.node`,
    `dev/Containerfile`, `go-build` / `go-test`; never commit an ELF (Scorecard
    Binary-Artifacts), never add pre-built download or fallback. Binding
    `rclonebypass_bpfel.go` IS committed; its embed is rewritten by
    `embed_generated_object.sh` to `embeddedObject()` (`object_embed.go`) so the package
    compiles without the object (locked `Go API Compatibility` gate builds it); no object ->
    `Start` fails `requireObject`, never a silent no-op. Pins `BPF_CLANG_VERSION` +
    `BPF_OBJECT_SHA256` in `build-config.env`; source / `vmlinux.h` / flag change ->
    re-record digest + commit regenerated binding same PR (little-endian only); use bpf2go struct mirrors, never hand-written
    layouts (old `mountPrefixT` was 264 B vs 260 B map value).
    `TestEmbeddedObjectMatchesMirrors`, `TestEmbeddedObjectMatchesPinnedDigest`,
    `TestRequireObject`, `TestEBPFStartFailsClosed`,
    `scripts/dev/tests/test_gen_node_bpf.py` guard.
    Kernel licence string (ADR-1559): `rclone_bypass.bpf.c` declares
    `"GPL"` (`SEC("license")`), SPDX stays `EUPL-1.2`; never `"Dual BSD/GPL"`
    (grant never made) or `"EUPL-1.2"` (GPL-only helpers refused, program
    never loads). Guard: `TestEmbeddedObjectLicence`;
    `TestEmbeddedObjectLoadsIntoKernel` on hosts passing `Preflight`.

16. **Metrics from `metricdef` only** (`metrics.go`, `main.go::nodeServerOptions`,
    WP16 / issue #2430): node families = `metricdef.ByEmitter(metricdef.Node)`,
    built via `observability.NewGauge/NewCounter/NewHistogram` on own registry
    (`provideNodeRegistry` -> `observability.NewRegistry`, ADR-1014). No
    `prometheus.New*Vec` / promauto here. Job metrics recorded in
    `controller_client.go::runJob` (`jobStarted` / `jobDone`), slots set in
    `provideControllerClient`. `nodeServerOptions` shared by `main` and
    `app_test.go::productionGraph`; tests set `VMAFX_HTTP_ADDR` to free port
    (`writeNodeEnv`), never bind `:9090`. Guards:
    `metrics_test.go::TestNodeServesEveryFamilyItEmits`,
    `TestControllerClientRecordsJobMetrics`, `TestNodeHTTPServesMetricsAndProbes`.

17. **Device memory + ScoreStream metrics** (`device_memory.go`, `metrics.go`):
    GPU memory read at scrape time: `cuda` -> `nvidia-smi --query-gpu=...
    --format=csv,noheader,nounits` (MiB), `hip` -> amdgpu sysfs
    `mem_info_vram_{used,total}` (bytes), other backends none (families
    registered, no series). Failed read -> `vmafx_metrics_read_errors_total
    {source="device_memory"}`, rest of page served; never fail /metrics.
    Tests inject reader (`newNodeMetrics`); never run real `nvidia-smi` in
    contract test. Stream sessions via `scoringservice.StreamMetrics`
    (`provideStreamMetrics`, set on handler in register invoke).
