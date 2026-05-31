### vmafx-mcp: Go implementation of the VMAFX MCP server

Added `cmd/vmafx-mcp/` — a single static Go binary that exposes the same 15
MCP tools as the Python `vmaf-mcp` server with byte-for-byte schema parity
(ADR-0704). Uses the official MCP Go SDK v1.6.1 (stdio and HTTP transports).
The Python server is preserved alongside; this is Stage 1 of the Go migration.

```bash
# Build
go build -o vmafx-mcp ./cmd/vmafx-mcp/

# Run (drop-in replacement for vmaf-mcp)
vmafx-mcp
```

Also added `pkg/libvmaf/` — shared path-resolution and allowlisting helpers
used by both `vmafx-mcp` and the parallel `vmafx-server` binary.
