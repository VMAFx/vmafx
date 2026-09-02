- Replace `go.uber.org/zap` with the standard-library `log/slog` package in
  `cmd/vmafx-operator/main.go` and its envtest suite. The operator was the
  last of 25 Go importers still using the kubebuilder-template-default zap
  logger; all other vmafx Go binaries (`vmafx-server`, `vmafx-controller`,
  `vmafx-tune`, MCP server, …) already log via `slog`. The operator now
  bridges `slog.NewJSONHandler` into controller-runtime via
  `logr.FromSlogHandler`, removing the last direct dependency on
  `go.uber.org/zap`. `zap` remains an indirect transitive dependency of
  `sigs.k8s.io/controller-runtime` internals; that is upstream's choice and
  unaffected by this change.
