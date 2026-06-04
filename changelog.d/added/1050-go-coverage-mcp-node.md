- `cmd/vmafx-mcp`: 23 unit tests covering arg helpers (`strArg` / `intArg` /
  `floatArg` / `boolArg` / `hasArg`), `handleListModels` filesystem walk,
  `handleListBackends` and `handleVmafVersion` fallback paths, `probeBackends`
  cache behaviour, and `handleVmafScore` path-validation error paths (ADR-1050).
- `cmd/vmafx-node`: 6 unit tests covering `classifyJob` (scoring / AI / unknown
  variants) and `Executor.Execute` nil-scorer / nil-aiReg error paths (ADR-1050).
