### Go test coverage round 2

Added targeted test files across five Go packages to push statement coverage
significantly above the round-1 baseline:

- `cmd/vmafx-server`: gRPC handler matrix (Score, Health, ScoreStream Phase-1
  stub validation, runGRPC/runGRPCWithServer lifecycle); REST adapter
  ScoreVideoPair happy + sad paths; Swagger UI try-it-out toggle +
  method-not-allowed guards. Coverage 52% → 78%.
- `cmd/vmafx-tune/cmd`: report subcommand (runReport Markdown + HTML,
  loadReportFile compare + ladder schemas, error paths, newReportCmd args
  validation). Coverage 63% → 73%.
- `pkg/libvmaf`: PATH fallback in New(), resolveModel absolute-path and
  .json-suffix branches, parseOutput corrupt-file, nil-context fast path,
  repeated Close(). Coverage 78% → 82%.
- `pkg/observability`: logInfo with non-nil logger, noopShutdown, InitOTel
  non-numeric sampler-arg, composite shutdown with cancelled context.
  Coverage 87% → 88%.
- `cmd/vmafx-operator/internal/controller`: coverage already at 74% via
  the existing envtest Ginkgo suite (KUBEBUILDER_ASSETS required).
