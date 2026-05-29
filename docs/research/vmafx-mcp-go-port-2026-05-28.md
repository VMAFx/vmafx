# Research digest: vmafx-mcp Go port (2026-05-28)

## Scope

This digest covers the evaluation of the official MCP Go SDK and the design
decisions for porting the Python MCP server to Go.

## MCP Go SDK evaluation

The official Go SDK (`github.com/modelcontextprotocol/go-sdk`) reached v1.6.1
as of 2026-05-28 (MIT license). It provides:

- `mcp.Server` with `AddTool`, `AddTool` (generic typed variant), raw
  `ToolHandler` interface.
- `mcp.StdioTransport` for stdin/stdout JSON-RPC (matches the Python
  `stdio_server()` context manager).
- `mcp.NewStreamableHTTPHandler` for HTTP transport (replaces the Python
  `http_transport.py` added in PR #1583).
- `mcp.NewInMemoryTransports()` for in-process testing without subprocess
  overhead — critical for `TestToolListMatchesPython`.
- `InputSchema any` field on `mcp.Tool`: the SDK accepts a `json.RawMessage`
  so we pre-serialise all schemas at startup via `mustSchema()`.

A hand-rolled JSON-RPC 2.0 implementation was evaluated and rejected (see
ADR-0704 alternatives table).

## Tool parity strategy

The Python server registers tools in `_list_tools()` and dispatches in
`_call_tool()`. The Go port separates these concerns into `registerTools()`
(tools.go) and individual `handle*` functions (impl.go). The schema
definitions in `registerTools()` were transcribed directly from the Python
`_list_tools()` return value, preserving enum sets, `required` arrays, and
`default` values exactly.

The `TestToolSchemasMatchPython` test validates the required-field sets for
all 10 tools that have non-trivial schemas.

## VLM gap

The Python server's `describe_worst_frames` tool uses HuggingFace
Transformers + SmolVLM / Moondream2 for artefact description. These are
Python-only dependencies. The Go port extracts frames via ffmpeg (same as
Python) and returns a placeholder string for the description. This is
documented in the tool's description field (visible to IDE clients) and in
the docs/mcp/index.md comparison table.

## ONNX evaluation delegation

`eval_model_on_split` and `compare_models` depend on onnxruntime (Python
wheel). The Go implementation delegates to an inline `python3 -c` script.
This preserves tool availability (IDE clients can call the tool and get a
result) while deferring a full Go ONNX binding to Stage 2.

## `pkg/libvmaf/` design

The shared helper package was designed to be importable by both `vmafx-mcp`
and the parallel `vmafx-server` agent's binary. It contains:

- `FindBinary()` — vmaf CLI resolution (mirrors Python `_vmaf_binary()`).
- `RepoRoot()` — walks upward from `os.Getwd()` until CLAUDE.md is found.
- `AllowedRoots()` — default allowlist + `VMAF_MCP_ALLOW` extension
  (mirrors Python `_allowed_roots()`).
- `ValidatePath()` — resolves symlinks and checks containment, returning
  the canonical absolute path on success.

The package intentionally does not include cgo bindings to libvmaf.so —
that is Stage 2 scope.
