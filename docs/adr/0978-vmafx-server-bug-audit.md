<!-- markdownlint-disable MD013 MD060 -->
# ADR-0978: vmafx-server + pkg/score bug-audit — shutdown leak, gRPC Send-EOF surfacing, HTTP body cap, panic recovery

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `security`, `bug`, `audit`, `go`, `grpc`, `http`, `server`

## Context

A deep-dive audit of `cmd/vmafx-server/` (Go gRPC + HTTP scoring service —
ADR-0703) and `pkg/score/` (gRPC client wrapper — ADR-0703 / ADR-0933)
found four reachable defects plus one defensive cleanup. None are
score-correctness bugs; all are reliability / robustness defects on
production-server surfaces.

1. **`pkg/observability.NewShutdownContext` goroutine + signal-handler
   leak.** The previous implementation spawned a goroutine blocked on
   `<-ch` with no `<-ctx.Done()` arm:

   ```go
   func NewShutdownContext() (context.Context, context.CancelFunc) {
       ctx, cancel := context.WithCancel(context.Background())
       go func() {
           ch := make(chan os.Signal, 1)
           signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT)
           defer signal.Stop(ch)
           <-ch
           cancel()
       }()
       return ctx, cancel
   }
   ```

   Documented contract: "the returned stop function must be called when
   the context is no longer needed to release resources." But calling
   `stop()` (the returned `cancel` func) only cancelled the parent
   context — it did NOT unblock the goroutine's `<-ch`, nor did it
   `signal.Stop(ch)`. Each `NewShutdownContext` / `stop()` cycle that
   exited via `stop()` first (i.e. any code path that did not receive an
   actual SIGTERM/SIGINT) leaked one goroutine plus one signal-handler
   subscription for the rest of the process lifetime. Reachable on every
   early-return path in `cmd/vmafx-server/main.go`
   (`scorer, err := libvmaf.New(...)` returning non-nil err →
   `os.Exit(1)` skips the `defer stop()`); the more subtle case is
   repeated test usage of NewShutdownContext as a primitive, where the
   leak accumulates per call.

2. **`pkg/score.Client.OpenScoreStream` + `ScoreStream.PushFrame` surface
   meaningless `io.EOF` instead of the server's real status.** Per the
   `grpc.ClientStream` contract, `Send` returns `io.EOF` "whenever the
   stream is done from the server's perspective" — including when the
   server has already half-closed the stream with a non-OK status (e.g.
   `codes.InvalidArgument` for a malformed `StreamConfig`). The actual
   status only surfaces on `Recv`. The pre-fix wrappers wrapped the
   bare EOF in `fmt.Errorf("score: send StreamConfig: %w", err)`, so a
   caller sending a malformed config saw `"score: send StreamConfig:
   EOF"` instead of `"InvalidArgument: ScoreStream: first message must
   set the config oneof"`. Triggers any time the server rejects an
   opening frame before reading client input — including the existing
   ScoreStream `width/height == 0` / `pixel_format == UNSPECIFIED` /
   wrong-oneof validators in `grpc_server.go:107-138`. The drag-along
   bug is in `PushFrame`, which has the same Send call shape and the
   same EOF path.

3. **`cmd/vmafx-server/http_server.handleScore` has no request-body size
   limit.** `json.NewDecoder(r.Body).Decode(&req)` reads until EOF.
   Combined with a public, unauthenticated POST endpoint (the current
   default — TLS / auth is deferred to a follow-up ADR), an attacker can
   POST a multi-GB body and balloon the decoder's read buffer until the
   process OOMs. The legitimate use case is a few-hundred-byte JSON
   blob (three string fields, longest field bounded by
   `PATH_MAX = 4096`). The cap that fits production-grade comfortably
   is 1 MiB.

4. **`cmd/vmafx-server/grpc_server` has no panic-recovery interceptor.**
   A panic inside any handler (the cgo libvmaf call path is the obvious
   risk surface, but any handler counts) tears down the gRPC worker
   goroutine; the gRPC library re-raises it in the connection's read
   loop, crashing the entire server process and dropping every
   in-flight request and stream on the floor. Production gRPC servers
   conventionally install `grpc.UnaryInterceptor` +
   `grpc.StreamInterceptor` recovery handlers that translate the panic
   into `codes.Internal` to the offending client while keeping the
   server alive.

5. **Defensive: `pkg/score.ScoreStream.Recv` compares `err == io.EOF`.**
   The current gRPC client surfaces io.EOF as the bare sentinel, so
   `==` works today. The defensive form `errors.Is(err, io.EOF)` keeps
   the wrapper's contract stable if a future gRPC release wraps the
   sentinel inside a status. Cheap to fix; rolled into the same PR.

The audit deliberately did **not** modify Netflix golden assertions
(CLAUDE.md §8), did not weaken any test threshold, and did not change
any scoring math. All five fixes are pure reliability / hardening.

Out of scope, deliberately deferred (would force a multi-package
signature change touching `pkg/libvmaf` + `cmd/vmafx-controller` +
`cmd/vmafx-node`): the `scorer.Score()` API takes no `context.Context`,
so a vmaf CLI subprocess keeps running after the HTTP / gRPC client
disconnects. Tracked separately.

## Decision

Apply five coordinated fixes in one PR:

- **`pkg/observability.NewShutdownContext`**: delegate to
  `signal.NotifyContext(context.Background(), syscall.SIGTERM,
  syscall.SIGINT)`. The stdlib idiom (Go 1.16+) unwinds correctly on
  both paths — signal arrival AND `stop()` called first — releasing
  both the goroutine and the signal-handler subscription.

- **`pkg/score.OpenScoreStream` + `PushFrame`**: introduce a
  `recvStatusOnEOF` helper that detects an `io.EOF` from `Send`, calls
  `Recv` to drain the underlying gRPC status, and returns it to the
  caller. Pass-through for any non-EOF Send error and for the case
  where Recv yields no usable status (fall back to the original EOF).

- **`cmd/vmafx-server/http_server.handleScore`**: cap the request body
  at 1 MiB via `http.MaxBytesReader`, map `*http.MaxBytesError` to
  HTTP 413 Request Entity Too Large with a typed
  `errorResponse{Error: ...}` body. Constant
  `maxScoreRequestBodyBytes = 1 << 20` lives at the package scope so
  the test can reference it.

- **`cmd/vmafx-server/grpc_server.runGRPC`**: install
  `recoveryUnaryInterceptor` + `recoveryStreamInterceptor` on the gRPC
  server. Each `defer-recover`s, logs the panic at ERROR with full
  stack, and returns `status.Errorf(codes.Internal, "internal server
  error")`. The server stays alive across panics.

- **`pkg/score.ScoreStream.Recv`**: replace `err == io.EOF` with
  `errors.Is(err, io.EOF)`. Defensive; behaviour-preserving today.

New regression tests:

- `TestNewShutdownContext_NoGoroutineLeak` (pkg/observability) — 100
  cycles of NewShutdownContext / stop(), then asserts
  `runtime.NumGoroutine()` returns to baseline (tolerance ≤ baseline+2
  for scheduler noise). Pre-fix would leak monotonically and trip.
- `TestOpenScoreStream_SurfaceServerStatusOnSendEOF` (pkg/score) —
  drives a stub server that immediately rejects ScoreStream with
  InvalidArgument and asserts the client wrapper surfaces
  InvalidArgument rather than EOF on either OpenScoreStream or
  PushFrame.
- `TestRecv_EOFAfterAggregateUsesErrorsIs` (pkg/score) — exercises Recv
  against the existing Phase-1 stub and asserts the Unimplemented
  status surfaces correctly (covers the errors.Is path change).
- `TestScoreEndpoint_RejectsOversizedBody` (cmd/vmafx-server) — POSTs a
  ~1.001 MiB body, asserts 413.
- `TestScoreEndpoint_AcceptsBoundedBody` (cmd/vmafx-server) — POSTs a
  64 KiB body, asserts NOT-413 (cap is permissive enough for
  realistic loads).
- `TestPanicRecovery_Unary` / `TestPanicRecovery_Stream`
  (cmd/vmafx-server) — register a panicking handler against the same
  interceptor stack and assert codes.Internal + server stays alive
  across a second call.

All tests pass under `go test -race -count=1`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix the observability leak in isolation (single-file PR) | Small surface, easy review | Three of the five defects share the same root cause (production-server hardening on Go surfaces nobody had audited end-to-end); shipping one fix at a time fragments the audit story. CLAUDE feedback ("PR hygiene: bigger PRs over micro-PRs") explicitly prefers the bundle | Bundle matches the user's stated PR-sizing preference |
| Use `oklog/run` or `errgroup`'s shutdown semantics instead of `signal.NotifyContext` | Slightly more featureful (timer-bounded shutdown) | Adds a dependency; existing main.go already uses `errgroup` for the runHTTP/runGRPC pair and that part stays unchanged | `signal.NotifyContext` is stdlib, zero-dep, and the bug is specifically about the signal-context primitive — keep the fix narrow |
| Translate Send-EOF inside the *generated* gRPC code (codegen patch) | Affects every Send call uniformly | Forks the protoc-gen-go-grpc generator; massive maintenance cost, breaks `make proto-gen` reproducibility | Translate at the wrapper layer only — that's exactly what `pkg/score` exists for |
| Run a panic-recovery middleware via `go-grpc-middleware/recovery` package | Battle-tested, single-line install | One more dependency (and a transitive on the v2 module that pulls in zap) | Hand-rolled 15-line recovery is trivial; avoiding a transitive zap pull keeps the binary smaller |
| Configure HTTP body cap globally via `http.Server.MaxBytesReader` | One config knob, applies to every handler | Applies to readyz/healthz too where the body is empty anyway; conceptually wrong for differently-shaped endpoints. The score endpoint is the only one that reads a body | Apply the cap at the only endpoint that consumes a body |
| Add TLS + bearer-token auth at the same time | Removes the unauthenticated-attacker assumption that motivates the body cap | Scope creep; TLS/auth is a separate decision that deserves its own ADR | Body cap is still right defence-in-depth even with auth (a compromised credential should not OOM the server) |
| `// nolint` the Send-EOF + Recv `==` lines instead of fixing | Zero risk of behaviour change | Surfaces leak to every caller; CLAUDE.md §12 rule 12 requires NOLINT only for load-bearing invariants and the EOF wrap is neither | Refactor over nolint |

## Consequences

- **Positive**: `vmafx-server` no longer leaks one goroutine + one
  signal-handler subscription per `NewShutdownContext` / `stop()`
  cycle; ScoreStream clients see the server's actual gRPC status when
  framing is rejected; the `/v1/score` endpoint can't be OOM-DoSed by
  an unauthenticated POST; any handler panic translates to
  `codes.Internal` rather than crashing the server; the Recv
  error-comparison is robust to a future gRPC release wrapping
  `io.EOF`.
- **Negative**: Six new test additions; one new package-scope constant
  (`maxScoreRequestBodyBytes`). The gRPC server now carries two
  interceptor wrappers; per-RPC cost is a single `defer recover()`
  (sub-microsecond).
- **Neutral / follow-ups**:
  - Scorer subprocess cancellation (`exec.CommandContext`) — deferred
    to a separate PR; requires extending `Scorer.Score` to take a
    `context.Context`, touching `pkg/libvmaf`,
    `cmd/vmafx-controller/{http_server,grpc_server}`, and
    `cmd/vmafx-node/executor`. New entry added to
    `docs/state.md → Open bugs` as
    `T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31`.
  - TLS + bearer-token auth on the public HTTP + gRPC endpoints —
    already tracked as future work in `pkg/score/grpc_client.go`
    comments (Dial defaults to insecure).
  - Optional follow-up: add a similar body cap to the
    `cmd/vmafx-controller` HTTP score endpoint (same shape; out of
    scope for this targeted audit).

## References

- Source: bug-audit task brief, 2026-05-31 session.
- ADR-0703 (vmafx-server Go gRPC + HTTP service) — the parent surface
  these defects live on.
- ADR-0933 (gRPC streaming multi-frame scoring) — the streaming
  surface whose Send/Recv ordering motivates the EOF-translation fix.
- gRPC Go docs: <https://pkg.go.dev/google.golang.org/grpc#ClientStream>
  — definitive statement that Send returns `io.EOF` when "the stream
  is done" and the underlying status comes via Recv.
- Go stdlib docs: <https://pkg.go.dev/os/signal#NotifyContext> — the
  idiomatic shutdown-context primitive (Go 1.16+).
- Companion audit shipped earlier in the same session: PR #495
  (`fix(core/tools)` input-reader safety, ADR-0977).
