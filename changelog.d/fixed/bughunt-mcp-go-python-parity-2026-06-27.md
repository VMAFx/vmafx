- MCP server: port the Python ADR-0967 HTTP hardening to the Go
  `cmd/vmafx-mcp` streamable-HTTP transport. New `http_security.go` adds a
  bearer-token auth middleware (`VMAFX_MCP_HTTP_TOKEN`, constant-time compare,
  `VMAFX_MCP_HTTP_NO_AUTH=1` opt-out, refuse-all when neither is set), a 4 MiB
  request-body limit (`http.MaxBytesReader` + Content-Length pre-flight → 413),
  and a loopback-only default bind (`VMAFX_MCP_HTTP_BIND`, default `127.0.0.1`,
  applied when `mcp.http.addr` carries no explicit host). The Go HTTP transport
  was previously unauthenticated, all-interfaces, and unbounded — diverging
  from the locked-down Python server.
- MCP server: align the score-precision default across all paths to `legacy`
  (`%.6f`, the documented C-CLI default per ADR-0119). The Python HTTP
  `/v1/score` path and the Go direct-cgo→subprocess fallback both defaulted to
  `"17"`, so a client got a different numeric format depending on which
  transport / dispatch path served the request.
- MCP server (Go `eval_model_on_split`): add the pred/target shape-mismatch
  guard the Python `_eval_model_on_split` already carries, so a model whose
  output rank does not match the target vector returns a clear `error` JSON
  instead of a misleading `pearsonr` failure or a silently-broadcast result.
- MCP server (Go vmaf-tune wrappers `run_compare` / `run_ladder` /
  `run_tune_per_shot`): capture the subprocess stderr and fold it into the
  wrapped error (`vmaf-tune <sub> exited <rc>: <stderr>`), mirroring the Python
  wrappers. The wrappers previously used `exec.Output()` and discarded stderr,
  losing the failure diagnostic vmaf-tune emits.
