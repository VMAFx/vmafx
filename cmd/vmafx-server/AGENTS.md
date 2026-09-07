# AGENTS.md — cmd/vmafx-server

Go gRPC + HTTP scoring service. See [ADR-0703](../../docs/adr/0703-vmafx-server-go-grpc.md).

## Rebase-sensitive invariants

1. **Proto-package stability** (`proto/vmafx.proto`): the proto stays in
   package `vmafx.v1`. The unary `Score` and `Health` RPCs are frozen for
   compatibility; never rename or remove their request / response fields.
   Additive surfaces (new RPCs, new messages, new enum variants) only.
   When breaking changes become necessary, bump the proto package to
   `vmafx.v2` and ship both side-by-side until the v1 deprecation
   window closes — never edit `vmafx.v1` in-place.

2. **`ScoreStream` opening message** (`proto/vmafx.proto` +
   `grpc_server.go`): the first `ScoreStreamRequest` on the bidirectional
   stream MUST set the `config` oneof (`StreamConfig`); every subsequent
   request MUST set `frame_pair`. The server validates this in
   `grpcServer.ScoreStream` and rejects malformed sequences with
   `codes.InvalidArgument` before reading any frame bytes. Per ADR-0933,
   `frame_index` is strictly monotonic from 0 — gaps are an error. The
   Phase 2 implementation MUST preserve this contract so existing
   clients keep working.

3. **`ScoreStream` per-frame scores are emitted after EOF** (`grpc_server.go`,
   ADR-0933 Phase 2): the handler ingests every `FramePair` into a
   `pkg/libvmaf.StreamScorer` first, and only after the client half-closes
   does it flush, harvest per-frame + pooled scores, and stream back the
   `FrameScore` messages followed by the terminal `AggregateScore`. This
   ordering is mandatory — temporal VMAF features (motion) only finalise at
   flush, so a per-frame score cannot be produced the instant a frame
   arrives. Do not "stream scores as frames come in"; that would corrupt
   motion-dependent results. The framing-validation block (invariant 2) stays
   intact at the top of the handler, and the streaming path acquires the same
   `ScoreLimiter` slot a unary `Score` does.

4. **`UnimplementedVmafxScoringServer` embedding** (`grpc_server.go`):
   the `grpcServer` struct embeds `vmafxv1.UnimplementedVmafxScoringServer`
   so future proto additions don't break the build. Do not remove the
   embed even after every RPC has a real implementation; the generator
   regenerates the unimplemented stub on every proto change.

5. **Panic-recovery interceptors are not optional** (`grpc_server.go`,
   ADR-0978): a panic inside any handler (notably the cgo libvmaf call
   path) MUST be converted into a `codes.Internal` reply, keeping the
   server alive across the bad request — otherwise one bad request tears
   down the gRPC worker goroutine and crashes the process. The golusoris
   `grpc.Module` (ADR-1119) keeps the process ALIVE — it bakes
   `go-grpc-middleware/v2` recovery interceptors into the `*grpc.Server`
   it constructs — but it installs them with NO recovery handler.
   ⚠ Verified against the pin (go-grpc-middleware/v2 v2.3.3): the
   default returns a `*recovery.PanicError` (a plain error), which gRPC
   maps to `codes.Unknown`, NOT `codes.Internal`. So on the production
   golusoris-served path a panic currently surfaces as `codes.Unknown`;
   the ADR-0978 `codes.Internal` mapping is BLOCKED on golusoris#225
   (interceptor / recovery-handler injection — the same gap that blocks
   the controller). The fork-local `recoveryUnaryInterceptor` /
   `recoveryStreamInterceptor` helpers (which DO map to `codes.Internal`)
   are retained for the package's own test harnesses (they build
   standalone `grpc.Server`s) and as the drop-in once #225 lands or if
   gRPC construction is ever moved back off golusoris. They MUST be
   carried through in either case.

6. **`POST /v1/score` body cap** (`http_server.go`, ADR-0978): the
   handler wraps `r.Body` in `http.MaxBytesReader(w, r.Body,
   maxScoreRequestBodyBytes)` (1 MiB) and maps `*http.MaxBytesError`
   to HTTP 413. This is defence-in-depth against unauthenticated POST
   DoS even after TLS / auth lands. If the legitimate request shape
   ever needs to exceed 1 MiB (e.g. inlined picture data), raise
   `maxScoreRequestBodyBytes` rather than removing the cap.

7. **R1 — scorer closes AFTER the gRPC server drains** (`main.go`,
   ADR-1119): the cgo `*libvmaf.Scorer` (and the per-call
   `StreamScorer` C contexts) must be released only after in-flight
   `Score` / `ScoreStream` RPCs have drained. The composition root
   guarantees this by forcing the scorer to be **constructed before**
   the golusoris `*grpc.Server` — there is an explicit
   `fx.Invoke(func(_ *libvmaf.Scorer) {})` registered ahead of the gRPC
   service-registration invoke, and that registration invoke lists the
   scorer-bearing `*grpcServer` before the `*grpc.Server` arg. Because
   fx runs OnStop hooks in reverse of construction order, the gRPC
   server's `GracefulStop` then runs before the scorer's `Close()`.
   `TestStopOrderScorerAfterGRPC` (`app_test.go`) pins this. Do NOT
   reorder those invokes, flip the arg order, or move the scorer's Close
   hook to a `*grpc.Server`-gated invoke — any of those inverts the
   construction order and closes the scorer while RPCs are still in
   flight (use-after-free of C resources).

8. **golusoris config sub-keys** (`main.go`, ADR-1119): the server reads
   its listen addresses from the golusoris HTTP/gRPC modules
   (`http.addr` → `VMAFX_HTTP_ADDR`, `grpc.listen` → `VMAFX_GRPC_LISTEN`)
   and its domain settings from `vmaf.binary` / `vmaf.model_dir` /
   `max_concurrent_scores`. These replace the pre-fx `VMAFX_PORT` /
   `VMAFX_GRPC_PORT` bare-port contract. The `fx.Replace(config.Options{
   EnvPrefix: "VMAFX_", ...})` line is load-bearing — without it the
   graph reads the framework default `APP_` prefix and ignores every
   `VMAFX_*` var.

9. **Release identity and runtime ABI** (`main.go`, `../../Dockerfile.go-server`,
   ADR-1129): release builds inject the published `vX.Y.Z` tag into
   `github.com/VMAFx/vmafx/pkg/version.version`. The exact two-argument
   `--version` path must return before constructing the fx graph or binding
   listeners; normal startup remains environment-only. The Dockerfile must
   build the fork's libvmaf, the CGO server, and the distroless runtime on the
   same Debian major ABI for both amd64 and arm64. Do not restore the distro
   `libvmaf-dev` builder or an architecture-specific library path: either can
   compile one binary against a different libvmaf from the one shipped at
   runtime.

10. **`bootstrap.HTTPTracing` sits next to `golusoris.HTTP`** (`main.go`,
    `app_test.go::productionGraph`, ADR-0782 / ADR-1119): it decorates the
    `http.Handler` golusoris's server module serves with the `otelhttp` span
    (`<METHOD> <path>`, probes and `/metrics` filtered), so every REST route
    is traced without per-route code. gRPC spans come from `grpcmod.Module`'s
    `otelgrpc` handler; OTel init from `bootstrap.Base` — the server has no
    private OTel code. `app_test.go::TestHTTPRouteEmitsServerSpan` and
    `TestOTelWiredThroughBootstrap` lock this; keep `productionGraph()` in
    step with `main.go` when either option list changes.
