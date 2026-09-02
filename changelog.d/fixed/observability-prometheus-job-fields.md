- **observability**: add missing `JobsSubmitted`, `JobsFailed`, `JobsCompleted` fields to the
  `Metrics` struct in `pkg/observability`. These fields were already initialised in `NewMetrics()`
  and consumed by the controller gRPC server, but were absent from the struct declaration,
  preventing the package from compiling. Fixes PR #534.
