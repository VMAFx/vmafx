# AGENTS.md — cmd/vmafx-mcp

Go MCP server that exposes 15 tools (vmaf_score, list_models, ...) to MCP
clients (Claude Desktop, Cursor, the in-tree gRPC server). Wraps the libvmaf
C library via two paths: the legacy `exec.Command(vmaf, ...)` subprocess
path (default) and the direct cgo path introduced by ADR-0931 (opt-in via
`VMAFX_MCP_DIRECT=1`).

## Rebase-sensitive invariants

1. **Tool name + schema parity with Python** (`tools.go`): every tool name
   and required-field set MUST match the Python `mcp-server/vmaf-mcp/`
   server's `_list_tools()` output. Adding a Go-only tool or renaming a
   required field breaks IDE MCP clients that were configured against the
   Python schema. `server_test.go::TestToolListMatchesPython` and
   `TestToolSchemasMatchPython` enforce this.

2. **Direct/subprocess dispatcher** (`impl.go`, `impl_direct.go`): every
   tool handler that has both paths MUST dispatch via
   `if directPathEnabled() { runFooDirect(...) } else { runFoo(...) }`.
   The direct variant is the responsible party for falling back to the
   subprocess variant on cases it does not handle (GPU backends, .onnx
   models, unresolvable model versions in Phase 1). This keeps the env
   gate safe to leave on globally. See ADR-0931 §"Fallback flag" and
   `docs/architecture/mcp-cgo-direct-migration.md`.

3. **`VMAFX_MCP_DIRECT` is strict** (`impl_direct.go::directPathEnabled`):
   only the exact string `"1"` enables the direct path. `"true"`, `"yes"`,
   `"on"` are all treated as off. Phase 3 may relax this, but until then
   the strict check is what
   `impl_direct_test.go::TestDirectPathEnabled` asserts.

4. **Marker stream** (`pkg/libvmaf.LogDirectPathSelected`): the
   "VMAFX_MCP_DIRECT=1 ... direct cgo scoring path" marker writes to
   stderr exactly once per process. Operators rely on it to confirm the
   path took effect. Routing it through slog or stdout breaks the
   convention.

5. **Response shape additions are additive only**: the direct path adds
   `backend_used = "cpu (direct cgo)"` and `frame_count` to the existing
   subprocess JSON shape. Phase 2/3 will populate `frames` and per-feature
   pooled scores. Existing keys (`pooled_metrics.vmaf.mean`,
   `backend_requested`, `mismatched_model_warning`) MUST remain
   identical to the subprocess output.

6. **Model arg compatibility** (`impl_direct.go::resolveModelArgToPath`):
   accepts the four MCP-level model forms (`version=NAME`, `path=ABS`,
   bare stem, abs/rel path). New forms require a coordinated update to
   the Python server's resolver (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).
