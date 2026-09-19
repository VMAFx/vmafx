---
name: add-mcp-tool
description: Scaffold a new VMAFX MCP tool handler with byte-compatible Go (cmd/vmafx-mcp) and Python (mcp-server/vmaf-mcp) implementations, registration, smoke tests, and docs/mcp/ page. Companion to /add-gpu-backend and /add-feature-extractor for the MCP surface.
---
<!-- markdownlint-disable MD013 -->

# /add-mcp-tool

Adds new MCP tool to **both** Go server (`cmd/vmafx-mcp`) and Python server
(`mcp-server/vmaf-mcp`).
Byte-for-byte parity per [`docs/mcp/tools.md`](../../../docs/mcp/tools.md) and
[ADR-0703](../../../docs/adr/0703-go-grpc-scoring-service.md).
Both servers must expose identical tool list, identical JSON-Schema,
identical response shape -> any IDE MCP client (Claude Desktop, Cursor)
switches transports without re-configuration.

## When to use

- Add tool delegating to `vmaf` CLI binary, `vmaf-tune` CLI, or libvmaf
  subprocess: `tools.go` and `vmaf_mcp/server.py` each add 1 handler.
- NOT for new transport (HTTP, stdio, SSE): server-level change -> use ADR
  process.
- NOT for non-tool resources / prompts: live under different
  registration paths.

## Invocation

```text
/add-mcp-tool <name>
```

`<name>` = `snake_case`, prefixed with subject domain.
Established prefixes: `vmaf_*` (scoring), `tune_*` (vmaf-tune),
`model_*` (model catalogue), `health_*` (server health).
Reject names colliding with existing tool in either server.

## Files created

| Path                                                                | Purpose                                            |
|---------------------------------------------------------------------|----------------------------------------------------|
| `cmd/vmafx-mcp/impl_<name>.go`                                      | Go handler stub                                    |
| `cmd/vmafx-mcp/impl_<name>_test.go`                                 | Go table-driven argument-validation test           |
| `mcp-server/vmaf-mcp/src/vmaf_mcp/tools/<name>.py`                  | Python handler stub                                |
| `mcp-server/vmaf-mcp/tests/test_<name>.py`                          | Python pytest covering schema + happy path         |
| `docs/mcp/tools/<name>.md`                                          | Per-tool human-readable doc page                   |
| `changelog.d/added/mcp-tool-<name>.md`                              | Changelog fragment                                 |

## Files patched

- `cmd/vmafx-mcp/tools.go` — append `addRawTool(srv, ...)` registration block
  with JSON-Schema input definition.
- `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` — append tool registration in
  `_list_tools()`, dispatch in `_call_tool()`.
- `docs/mcp/tools.md` — append row to master tool table (sorted by domain
  prefix, then name).
- `docs/mcp/index.md` — bump tool-count in overview paragraph.
- `cmd/vmafx-mcp/AGENTS.md` + `mcp-server/AGENTS.md` — note parity contract
  entry under "Tools currently shipped" (see ADR-0703 invariants).

## Workflow

1. Validate `<name>` matches `^[a-z]+_[a-z0-9_]+$`, unique in both servers
   (grep `cmd/vmafx-mcp/tools.go` and
   `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`).
2. Copy templates; substitute `@NAME@`, `@NAME_UPPER@`, `@NAME_PASCAL@`,
   `@COPYRIGHT@` placeholders.
3. Apply registration patches (idempotent: refuse if registration block
   already exists).
4. Run Go test (`go test ./cmd/vmafx-mcp/...`) and Python test
   (`pytest mcp-server/vmaf-mcp/tests/test_<name>.py`): confirm stubs compile,
   schema parses on both sides.
5. Run `scripts/mcp/parity-check.sh` (if present): confirm both servers
   advertise tool with identical schema bytes.
6. Open PR checklist comment:
   - Implementation TODO list (real CLI invocation, output parsing, error
     mapping).
   - `isError=True` reminder for failure paths (see memory entry
     `project_mcp_iserror_must_be_true`).
   - Per-surface doc bar (ADR-0100): `docs/mcp/tools/<name>.md` page MUST
     describe input schema, output schema, example invocation, error modes,
     security considerations before PR merges.

## Guardrails

- **Never** ship tool to only one server. Parity = contract. If Python side
  cannot yet implement (e.g. depends on Go-only dependency), Python handler
  raises `NotImplementedError` with TODO comment AND docs page calls out gap
  explicitly.
- **Never** set `isError=False` on failure path. Python `tools/<name>.py`
  template hard-codes helper asserting this at registration time.
- **Never** overwrite existing files. Scaffold refuses if target path
  already exists.
- **Never** add tool without doc page: per-surface doc rule in
  [ADR-0100](../../../docs/adr/0100-project-wide-doc-substance-rule.md) makes
  PR unmergeable without it.

## References

- [ADR-0703](../../../docs/adr/0703-go-grpc-scoring-service.md) — Go service
  surface
- [ADR-0005](../../../docs/adr/0005-embedded-mcp-transport.md) — embedded MCP
  transport
- [`docs/mcp/tools.md`](../../../docs/mcp/tools.md) — master tool reference
- [`cmd/vmafx-mcp/tools.go`](../../../cmd/vmafx-mcp/tools.go) — Go registry
- [`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`](../../../mcp-server/vmaf-mcp/src/vmaf_mcp/server.py)
  — Python registry
