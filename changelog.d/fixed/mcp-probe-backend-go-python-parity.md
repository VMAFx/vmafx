- MCP `probe_backend` (Go server, `cmd/vmafx-mcp/impl.go`): bump the synthetic
  probe frame from 32x32 to 64x64 4:2:0 8-bit, matching the Python server
  (`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`). 32x32 is below the CUDA ADM
  minimum of 36px per dimension, so on a CUDA build the probe silently received
  a null `vmaf.mean` score while the Go handler still reported
  `runtime_healthy=true` — a false-healthy signal and a Go↔Python byte-compat
  violation.
- Tighten the Go `runtime_healthy` predicate to mirror Python: a null or
  non-finite score now yields `runtime_healthy=false` with the error string
  `"vmaf returned exit 0 but score was null"` instead of an unconditional true.
- Refresh the stale "32x32" probe-size references in `impl.go`, `tools.go`,
  `server.py`, and `docs/mcp/tools.md` to "64x64" and document the >=36px
  CUDA-ADM minimum alongside the probe-size description.
