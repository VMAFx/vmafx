# AGENTS.md — cmd/vmafx-server

Go gRPC + HTTP scoring service. See
[ADR-0703](../../docs/adr/0703-vmafx-server-go-grpc.md).

## Rebase-sensitive invariants

1. **Proto-package stability** (`proto/vmafx.proto`): proto stays in package
   `vmafx.v1`. Unary `Score` and `Health` RPCs frozen for compatibility;
   never rename or remove request / response fields. Additive surfaces (new
   RPCs, new messages, new enum variants) only. Breaking change -> bump proto
   package to `vmafx.v2`, ship side-by-side until v1 deprecation closes; never
   edit `vmafx.v1` in-place.

2. **`ScoreStream` opening message** (`proto/vmafx.proto` + `grpc_server.go`):
   first `ScoreStreamRequest` on bidirectional stream MUST set `config` oneof
   (`StreamConfig`); every subsequent request MUST set `frame_pair`. Server
   validates in `grpcServer.ScoreStream`, rejects malformed sequences with
   `codes.InvalidArgument` before reading frame bytes. ADR-0933: `frame_index`
   strictly monotonic from 0 — gaps = error. Phase 2 implementation MUST
   preserve contract so existing clients keep working.

3. **`ScoreStream` per-frame scores are emitted after EOF** (`grpc_server.go`,
   ADR-0933 Phase 2): handler ingests every `FramePair` into
   `pkg/libvmaf.StreamScorer` first; client half-closes -> flush, harvest
   per-frame + pooled scores, stream back `FrameScore` messages then terminal
   `AggregateScore`. Ordering mandatory: temporal VMAF features (motion)
   finalise at flush -> per-frame score impossible on arrival. Do not "stream
   scores as frames come in"; corrupts motion-dependent results.
   Framing-validation block (invariant 2) intact at top of handler; streaming
   path acquires same `ScoreLimiter` slot unary `Score` does.

4. **`UnimplementedVmafxScoringServer` embedding** (`grpc_server.go`):
   `grpcServer` struct embeds `vmafxv1.UnimplementedVmafxScoringServer` ->
   future proto additions do not break build. Do not remove embed even after
   every RPC has real implementation; generator regenerates unimplemented
   stub on every proto change.

5. **Panic-recovery interceptors are not optional** (`grpc_server.go`,
   ADR-0978): handler panic (notably cgo libvmaf call path) MUST convert to
   `codes.Internal` reply, keeping server alive across bad request — otherwise
   bad request tears down gRPC worker goroutine, crashes process. golusoris
   `grpc.Module` (ADR-1119) keeps process ALIVE (bakes `go-grpc-middleware/v2`
   recovery interceptors into constructed `*grpc.Server`), but installs them
   with NO recovery handler. ⚠ Verified against pin (go-grpc-middleware/v2
   v2.3.3): default returns `*recovery.PanicError` (plain error); gRPC maps to
   `codes.Unknown`, NOT `codes.Internal`. Production golusoris path panic
   surfaces as `codes.Unknown`; ADR-0978 `codes.Internal` mapping BLOCKED on
   golusoris#225 (interceptor / recovery-handler injection; blocks controller).
   Fork-local `recoveryUnaryInterceptor` / `recoveryStreamInterceptor` helpers
   (map to `codes.Internal`) retained for package test harnesses (build
   standalone `grpc.Server`s) and drop-in once #225 lands or if gRPC
   construction moves off golusoris. MUST be carried through in either case.

6. **`POST /v1/score` body cap** (`http_server.go`, ADR-0978): handler wraps
   `r.Body` in `http.MaxBytesReader(w, r.Body, maxScoreRequestBodyBytes)`
   ` (1 MiB) and maps ` `*http.MaxBytesError` to HTTP 413. Defence-in-depth
   against unauthenticated POST DoS even after TLS / auth. If legitimate
   request needs > 1 MiB (e.g. inlined picture data), raise
   `maxScoreRequestBodyBytes`, do not remove cap.

7. **R1 — scorer closes AFTER the gRPC server drains** (`main.go`, ADR-1119):
   cgo `*libvmaf.Scorer` (and per-call `StreamScorer` C contexts) must release
   only after in-flight `Score` / `ScoreStream` RPCs drain. Composition root
   guarantees this: scorer constructed before golusoris `*grpc.Server` —
   explicit `fx.Invoke(func(_ *libvmaf.Scorer) {})` ahead of gRPC service
   registration invoke; registration invoke lists scorer-bearing `*grpcServer`
   before `*grpc.Server` arg. fx runs OnStop hooks in reverse construction
   order -> gRPC server `GracefulStop` runs before scorer `Close()`.
   `TestStopOrderScorerAfterGRPC` (`app_test.go`) pins this. Do NOT reorder
   invokes, flip arg order, or move scorer Close hook to `*grpc.Server`-gated
   invoke — inverts construction order, closes scorer while RPCs in flight
   (use-after-free of C resources).

8. **golusoris config sub-keys** (`main.go`, ADR-1119): server reads listen
   addresses from golusoris HTTP/gRPC modules (`http.addr` ->
   `VMAFX_HTTP_ADDR`, `grpc.listen` -> `VMAFX_GRPC_LISTEN`); domain settings
   from `vmaf.binary` / `vmaf.model_dir` / `max_concurrent_scores`. Replaces
   pre-fx `VMAFX_PORT` / `VMAFX_GRPC_PORT` bare-port contract.
   `fx.Replace(config.Options{EnvPrefix: "VMAFX_", ...})` line load-bearing —
   without it graph reads framework default `APP_` prefix, ignores `VMAFX_*`
   vars.

9. **Release identity and runtime ABI** (`main.go`,
   `../../Dockerfile.go-server`, ADR-1129): release builds inject published
   `vX.Y.Z` tag into `github.com/VMAFx/vmafx/pkg/version.version`. Exact
   two-argument `--version` path MUST return before constructing fx graph or
   binding listeners; normal startup environment-only. Dockerfile MUST build
   fork libvmaf, CGO server, distroless runtime on same Debian major ABI for
   both amd64 and arm64. Do not restore distro `libvmaf-dev` builder or
   architecture-specific library path: either compiles binary against
   different libvmaf from runtime.

10. **`bootstrap.HTTPTracing` sits next to `golusoris.HTTP`** (`main.go`,
    `app_test.go::productionGraph`, ADR-0782 / ADR-1119): decorates
    `http.Handler` golusoris server module serves with `otelhttp` span
    (`<METHOD> <path>`, probes and `/metrics` filtered) -> every REST route
    traced without per-route code. gRPC spans from `grpcmod.Module` `otelgrpc`
    handler; OTel init from `bootstrap.Base` — server has no private OTel
    code. `app_test.go::TestHTTPRouteEmitsServerSpan` and
    `TestOTelWiredThroughBootstrap` lock this; keep `productionGraph()` in step
    with `main.go` when option lists change.
