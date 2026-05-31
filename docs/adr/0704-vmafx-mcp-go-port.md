<!-- markdownlint-disable MD060 -->
# ADR-0704: vmafx-mcp Go port (JSON-RPC, stdio transport)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `mcp`, `go`, `build`, `agents`

## Context

The existing Python MCP server (`mcp-server/vmaf-mcp/`) works well but carries
a Python dependency chain (anyio, httpx, mcp, onnxruntime, pandas, scipy,
transformers, torch) that complicates deployment. Phase 4 of the VMAFX
modernisation (language consolidation) targets Go as the primary systems
language. A single static Go binary simplifies distribution, eliminates the
Python-wheel dependency at startup, and enables the `vmafx-server` and
`vmafx-mcp` binaries to share a single `pkg/libvmaf` cgo package.

The port is Stage 1: the Go binary is added alongside the Python server; the
Python implementation is not removed. IDE clients (Claude Desktop, Cursor)
remain unaffected because the tool names, argument schemas, and response shapes
are byte-for-byte compatible with the Python server.

The Phase 4 umbrella decision established Go as the target language for new
binaries in this fork. This ADR documents the specific decisions for the MCP
server port.

## Decision

We will ship a Go implementation of the VMAFX MCP server at
`cmd/vmafx-mcp/`. The Go module root is `go.mod` at the repository root
(module `github.com/VMAFx/vmafx`). The implementation uses the official
MCP Go SDK (`github.com/modelcontextprotocol/go-sdk v1.6.1`, MIT license)
rather than a hand-rolled JSON-RPC 2.0 implementation. All 15 tools exported
by the Python server are registered with identical names, required-field sets,
and response shapes. The default transport is stdio; HTTP (streamable MCP
transport) is available via `--transport http --port PORT`. The Go server
delegates scoring to the vmaf CLI binary and does not link libvmaf.so at
runtime — matching the Python approach exactly.

`pkg/libvmaf/` provides the shared path-resolution and allowlisting logic used
by both `vmafx-mcp` and the parallel `vmafx-server` binary. VLM (vision-language
model) descriptions are not available in the Go implementation and return a
clear "Go implementation does not include vlm extras" message consistent with
the Python server's "vlm extras not installed" fallback.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Hand-rolled JSON-RPC 2.0 | No external dep, full control | ~400 LOC of boilerplate; drift from MCP spec over time | MCP Go SDK is now stable (v1.6.1); maintenance burden not justified |
| Keep Python only | Zero migration effort | Blocks Phase 4 language consolidation; Python wheel chain stays in critical path | ADR-0701 direction |
| Port to Rust | Strong type safety, zero-cost abstractions | No existing cgo bridging story; no team familiarity | Go is the Phase 4 decision |
| Compile-in libvmaf via cgo | Eliminates subprocess overhead | Requires full cgo build pipeline; complicates cross-compilation and distribution | Out of scope for Stage 1; `pkg/libvmaf/` is designed to add this later |

## Consequences

- **Positive**: single static binary (`vmafx-mcp`), no Python wheel chain at
  startup; shared `pkg/libvmaf/` with `vmafx-server`; IDE clients continue to
  work unchanged.
- **Negative**: VLM descriptions (`describe_worst_frames`) return a placeholder
  in the Go implementation — users who need VLM output must use the Python
  server until Stage 2 adds a native VLM bridge.
- **Neutral / follow-ups**: Stage 2 will remove the Python server once the Go
  binary reaches parity (including VLM support). `eval_model_on_split` and
  `compare_models` delegate to a Python3 subprocess for ONNX evaluation —
  this should be replaced with a pure-Go ONNX runtime (e.g. `ort.InferenceSession`
  via `github.com/microsoft/onnxruntime-go`) in Stage 2.

## References

- Phase 4 language-modernisation decision (VMAFX rebrand plan, 2026-05-28).
- `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` — Python reference implementation.
- [MCP Go SDK](https://github.com/modelcontextprotocol/go-sdk) v1.6.1.
- [MCP spec](https://modelcontextprotocol.io/specification).
- Related PR: `feat/vmafx-mcp-go-port`.
