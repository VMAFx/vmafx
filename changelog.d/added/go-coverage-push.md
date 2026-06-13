- **Go test coverage push** (`cmd/vmafx-*`, `pkg/`): table-driven tests added
  for the Go binaries and packages with lowest coverage. Key gains:
  `cmd/vmafx-operator/internal/controller` 7→45 %, `pkg/observability` 68→87 %,
  `pkg/score` 47→68 %, `cmd/vmafx-controller/queue` 66→82 %,
  `cmd/vmafx-node` 30→46 %. Pre-existing build failures fixed (gRPC server
  undefined-var bug, operator int32 type mismatch, stale test API calls,
  MCP Vulkan backend missing from dispatcher).
