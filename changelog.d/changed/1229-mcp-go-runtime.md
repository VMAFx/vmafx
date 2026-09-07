- **The MCP server is now the Go binary `vmafx-mcp`; the Python package is
  deprecated** (ADR-1229). The fork carried two complete MCP implementations —
  `mcp-server/vmaf-mcp/` (Python, 25 files, 15,091 lines) and `cmd/vmafx-mcp/`
  (Go) — and only the Python one was wired up, while the Go binary was already
  built and installed at `/usr/local/bin/vmafx-mcp` in every container image and
  went unused. Their tool surfaces were compared directly and are identical: the
  same fifteen tools, the same names, and the same required-argument sets for the
  ten tools that take arguments. Attach with `docker exec -i vmaf-dev-mcp
  vmafx-mcp`; `dev/Containerfile` no longer runs `pip install -e` for the Python
  package. The Python tree is retained for one release as a reference
  implementation and marked deprecated — a follow-up removes it.
