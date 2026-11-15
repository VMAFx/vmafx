<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/vmafx-mcp

Go MCP server: exposes 24 tools (`vmaf_score`, `list_models`, ...) to MCP clients (Claude Desktop, Cursor, in-tree gRPC server). Wraps libvmaf C library via 2 paths: legacy `exec.Command(vmaf, ...)` subprocess path (default), direct cgo path from ADR-0931 (opt-in via `VMAFX_MCP_DIRECT=1`).
