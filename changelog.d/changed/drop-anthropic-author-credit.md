Drop the `Claude (Anthropic)` author entry from all six fork `pyproject.toml`
files (`ai`, `dev-llm`, `mcp-server/vmaf-mcp`, `tools/ensemble-training-kit`,
`tools/vmaf-roi-score`, `tools/vmaf-tune`); Anthropic is not a rights holder
(copyright-policy decision 2026-05-27). Also remove a stray 1 KiB whitespace
`tools/vmaf-tune/-version` artifact and correct a stale "16 tools" comment in
`cmd/vmafx-mcp/tools.go` (the server registers 15).
