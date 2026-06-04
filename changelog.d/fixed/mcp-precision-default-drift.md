Fix MCP precision-default drift: change probe --precision 6 (→%.6g) and schema
default "17" (→%.17g) to "legacy" (→%.6f) in both Go (cmd/vmafx-mcp) and Python
(mcp-server/vmaf-mcp) MCP surfaces, matching the C CLI default per ADR-0119
so that MCP output is Netflix-compatible without explicit precision argument (ADR-1038).
