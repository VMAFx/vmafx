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

6. **Model arg compatibility + allowlist enforcement**
   (`impl_direct.go::resolveModelArgToPath`): accepts the four MCP-level
   model forms (`version=NAME`, `path=ABS`, bare stem, abs/rel path).
   Every resolved path — including those supplied via `path=<abs>` or a
   bare absolute path — MUST be passed through `libvmaf.ValidatePath`
   before being returned.  Bypassing `ValidatePath` for absolute inputs
   allows an MCP caller to read arbitrary host files.  New forms require a
   coordinated update to the Python server's resolver
   (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).  Regression test:
   `TestResolveModelArgToPath_AllowlistEnforced`.

7. **gosec G304 / G204 contract** (every `os.ReadFile` / `os.Open` /
   `exec.Command*` in `impl.go`): any path or command variable consumed
   by these calls MUST either (a) round-trip through
   `libvmaf.ValidatePath` (caller-supplied paths), (b) originate from
   `os.CreateTemp` / `os.MkdirTemp` (locally-generated temp paths), or
   (c) come from `libvmaf.FindBinary` / `findVmafTune` / an
   `exec.LookPath` of a fixed binary name. Adding a new subprocess call
   site or file read without one of these gates means the new path is
   directly attacker-influenced; either add the validation or annotate
   `// #nosec G204` / `// #nosec G304` with a citation that names the
   protecting helper. The CI gate (`gosec -exclude-generated` in
   `go-ci.yml`) blocks the merge until one of the two is true.
   `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal`
   pins the `describeModel` allowlist; equivalent regressions for new
   tools belong next to it. See
   [ADR-0983](../../docs/adr/0983-gosec-findings-fix-sweep.md).

8. **Handler test coverage** (`impl_handlers_test.go`): the file covers the
   "binary not found" fast-fail branches of every tool handler, the
   `findVmafTune` env-override, `parseArgs` nil/valid/invalid, the ambiguous
   model-stem path in `describeModel`, and the real filesystem walks for
   `handleListModels` and `handleListExtractors`. When adding a new tool
   handler: add at least one error-path test that does not require an external
   binary (use `t.Setenv("VMAF_BIN", "/nonexistent/...")` or similar).
   Statement coverage target: 50 %+ on the `cmd/vmafx-mcp` package.
