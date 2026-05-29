- Go workspace audit (2026-05-29): added missing `modernc.org/sqlite` dependency
  (used by `cmd/vmafx-controller/queue` but absent from `go.mod`); added
  `JobsSubmitted`, `JobsCompleted`, `JobsFailed` counters and
  `SetControllerSources` gauge method to `pkg/observability.Metrics`; fixed
  stale `executor_test.go` / `main_test.go` in `cmd/vmafx-node` whose
  signatures referenced the old 3-arg `NewExecutor` and undefined `loadConfig`
  / `node` / `ScoringJob` symbols; wired the already-implemented `newLadderCmd`
  into `cmd/vmafx-tune/cmd/root.go` (it was shadowed by a stub); removed
  `ladder` from the stub-subcommand test in `compare_test.go`. `go vet` and
  `go test -race ./cmd/... ./pkg/...` are now clean on all non-envtest packages.
