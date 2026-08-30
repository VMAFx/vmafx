### Fixed

- **Go static-analysis sweep on `cmd/` + `pkg/`** — ran `nilness`,
  `staticcheck`, and `gosec` against fork-added Go code; fixed all real
  findings while preserving behaviour. Changes: replaced deprecated
  `prometheus.NewGoCollector` / `NewProcessCollector` with the
  `collectors.*` package in `cmd/vmafx-server` and `cmd/vmafx-controller`
  (SA1019 ×4); deleted dead `mockController` scaffolding from
  `cmd/vmafx-node/main_test.go` and an unused `mockScoreFunc` from
  `pkg/bisect/bisect_test.go` (U1000 ×8); simplified a redundant nil
  guard in `pkg/ai/infer_test.go` (S1009). Added a defensive
  containment-check around `os.ReadFile` inside the MCP feature-extractor
  walk (`cmd/vmafx-mcp/impl.go`) to neutralise the gosec G122 symlink-TOCTOU
  warning, and annotated two false-positive G118 graceful-shutdown
  goroutines (the canonical `net/http` shutdown pattern intentionally uses
  `context.Background()` because the request-scoped context is the one
  being cancelled). Final scoreboard: nilness 0, staticcheck 0, gosec 0.
