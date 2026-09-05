- **MCP schema defect fixes and parity cleanups**:
  Accept bitdepth 16 on scoring tools matching libvmaf capabilities; reconcile
  `precision` tool schema description to document default `legacy` (%.6f) and `max`
  lossless output (%.17g) per ADR-0119; remove stale Vulkan references following
  ADR-0726; fix stale "16 tools" count comment in Go `cmd/vmafx-mcp/tools.go` with
  exact 15-tool categorization derivation; and admit `python/test/resource/yuv` in
  allowed roots for worktree test runners.
