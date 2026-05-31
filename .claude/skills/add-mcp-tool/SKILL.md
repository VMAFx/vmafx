---
name: add-mcp-tool
description: Scaffold a new VMAFX MCP tool handler with byte-compatible Go (cmd/vmafx-mcp) and Python (mcp-server/vmaf-mcp) implementations, registration, smoke tests, and docs/mcp/ page. Companion to /add-gpu-backend and /add-feature-extractor for the MCP surface.
---

<!-- markdownlint-disable MD013 -->

# /add-mcp-tool

Adds a new MCP tool to **both** the Go server (`cmd/vmafx-mcp`) and the Python
server (`mcp-server/vmaf-mcp`) so the two stay byte-for-byte parity per the
contract in [`docs/mcp/tools.md`](../../../docs/mcp/tools.md) and
[ADR-0703](../../../docs/adr/0703-go-grpc-scoring-service.md). Both servers must
expose the identical tool list, identical JSON-Schema, and identical response
shape for any IDE MCP client (Claude Desktop, Cursor) to switch transports
without re-configuration.

## When to use

- Adding a new tool that delegates to the `vmaf` CLI binary, the `vmaf-tune`
  CLI, or a libvmaf subprocess — `tools.go` and `vmaf_mcp/server.py` both grow
  by one handler.
- NOT for adding a new transport (HTTP, stdio, SSE) — that is a server-level
  change; use the ADR process.
- NOT for adding non-tool resources / prompts (those live under different
  registration paths).

## Invocation

```text
/add-mcp-tool <name>
```

`<name>` is `snake_case`, prefixed with the subject domain. Established
prefixes: `vmaf_*` (scoring), `tune_*` (vmaf-tune), `model_*` (model
catalogue), `health_*` (server health). Reject names that collide with an
existing tool in either server.

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
  with the JSON-Schema input definition.
- `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` — append tool registration in
  `_list_tools()` and dispatch in `_call_tool()`.
- `docs/mcp/tools.md` — append a row to the master tool table (sorted by
  domain prefix, then name).
- `docs/mcp/index.md` — bump the tool-count in the overview paragraph.
- `cmd/vmafx-mcp/AGENTS.md` + `mcp-server/AGENTS.md` — note the parity
  contract entry under "Tools currently shipped" (see ADR-0703 invariants).

## Workflow

1. Validate `<name>` matches `^[a-z]+_[a-z0-9_]+$` and is unique in both
   servers (grep `cmd/vmafx-mcp/tools.go` and
   `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`).
2. Copy templates and substitute `@NAME@`, `@NAME_UPPER@`, `@NAME_PASCAL@`,
   and `@COPYRIGHT@` placeholders.
3. Apply the registration patches (idempotent — refuse if the registration
   block already exists).
4. Run the Go test (`go test ./cmd/vmafx-mcp/...`) and Python test
   (`pytest mcp-server/vmaf-mcp/tests/test_<name>.py`) to confirm the stubs
   compile and the schema parses on both sides.
5. Run `scripts/mcp/parity-check.sh` (if present) to confirm both servers
   advertise the new tool with identical schema bytes.
6. Open a PR checklist comment with:
   - Implementation TODO list (real CLI invocation, output parsing, error
     mapping).
   - `isError=True` reminder for failure paths (see memory entry
     `project_mcp_iserror_must_be_true`).
   - Per-surface doc bar (ADR-0100): the `docs/mcp/tools/<name>.md` page MUST
     describe input schema, output schema, example invocation, error modes,
     and security considerations before the PR can merge.

## Guardrails

- **Never** ship a tool to only one server. Parity is the contract — if the
  Python side can't yet implement (e.g. depends on a Go-only dependency),
  the Python handler raises `NotImplementedError` with a TODO comment AND
  the docs page calls out the gap explicitly.
- **Never** set `isError=False` on a failure path. The Python `tools/<name>.py`
  template hard-codes the helper that asserts this at registration time.
- **Never** overwrite existing files. Scaffold refuses if any target path
  already exists.
- **Never** add a tool without the doc page — the per-surface doc rule in
  [ADR-0100](../../../docs/adr/0100-project-wide-doc-substance-rule.md) makes
  the PR unmergeable without it.

## References

- [ADR-0703](../../../docs/adr/0703-go-grpc-scoring-service.md) — Go service
  surface
- [ADR-0005](../../../docs/adr/0005-embedded-mcp-transport.md) — embedded MCP
  transport
- [`docs/mcp/tools.md`](../../../docs/mcp/tools.md) — master tool reference
- [`cmd/vmafx-mcp/tools.go`](../../../cmd/vmafx-mcp/tools.go) — Go registry
- [`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`](../../../mcp-server/vmaf-mcp/src/vmaf_mcp/server.py) — Python registry
