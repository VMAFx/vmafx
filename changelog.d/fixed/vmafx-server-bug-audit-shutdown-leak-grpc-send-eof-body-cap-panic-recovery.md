- **`vmafx-server` + `pkg/score` bug-audit fixes (ADR-0978)**:
  - **`pkg/observability.NewShutdownContext` no longer leaks one
    goroutine + one signal-handler subscription per call when `stop()`
    is invoked before a signal arrives.** The previous implementation
    spawned a goroutine blocked on `<-ch` with no `<-ctx.Done()` arm;
    early-exit paths in `main()` (e.g. `libvmaf.New` returns err →
    `os.Exit(1)` skips the `defer stop()`) accumulated leaks for the
    process lifetime. Fixed by delegating to the stdlib
    `signal.NotifyContext` (Go 1.16+).
  - **`pkg/score.OpenScoreStream` / `ScoreStream.PushFrame` now surface
    the server's actual gRPC status when `Send` returns `io.EOF`.**
    Previously a malformed `StreamConfig` (e.g. zero dimensions, wrong
    oneof) caused the wrapper to return `"score: send StreamConfig:
    EOF"` instead of `"InvalidArgument: ScoreStream: first message
    must set the config oneof"`. A `recvStatusOnEOF` helper drains
    `Recv` on Send-EOF and returns the real status to the caller.
  - **`cmd/vmafx-server` POST `/v1/score` now caps the request body at
    1 MiB** via `http.MaxBytesReader`. Without the cap, an
    unauthenticated POST with a multi-GB body could balloon the JSON
    decoder's read buffer until the process OOMed. Bodies above the
    cap return HTTP 413 Request Entity Too Large.
  - **`cmd/vmafx-server` gRPC server now installs unary + stream
    panic-recovery interceptors.** A panic in any handler (notably the
    cgo libvmaf call path) previously crashed the entire server
    process; the interceptors convert the panic into `codes.Internal`
    to the offending client while keeping the server alive.
  - **`pkg/score.ScoreStream.Recv` now uses `errors.Is(err, io.EOF)`**
    instead of `err == io.EOF`. Defensive — keeps wrapper EOF
    semantics stable if a future gRPC release wraps `io.EOF` inside a
    status.

  Regression tests added under
  `pkg/observability/observability_test.go`,
  `pkg/score/grpc_client_test.go`,
  `cmd/vmafx-server/main_test.go`, and
  `cmd/vmafx-server/grpc_recovery_test.go`. All pass under
  `go test -race -count=1`.
