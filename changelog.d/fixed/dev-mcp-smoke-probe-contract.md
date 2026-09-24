- **The periodic dev-MCP smoke probe again measures the backends it names.**
  It now uses the current exclusive `--backend` CLI with complete raw-YUV
  geometry, reads a real JSON score and verifies `backend_used`, and talks to
  the production `vmafx-mcp` Go server with the required initialization
  handshake and current tool schemas while keeping the stdio session open
  until each response arrives. Backend failures remain valid JSON even when
  their diagnostics contain quotes or control characters.
