## Added

- **MCP server direct cgo scoring path (Phase 1)** behind
  `VMAFX_MCP_DIRECT=1`: `pkg/libvmaf.ScoreDirect` and
  `pkg/libvmaf.ValidateModel` invoke libvmaf in-process via cgo, replacing
  the `exec.Command(vmaf, ...)` + parse-stdout flow for the `vmaf_score`
  and `describe_model` MCP tools. Subprocess path remains the default and
  is the transparent fallback for cases the direct path does not yet
  handle (GPU backends, `.onnx` models, unresolved model versions). Adds
  typed errors `ErrInvalidArgument`, `ErrOutOfMemory`, `ErrModelNotFound`,
  `ErrPictureRead` (mapped from libvmaf negative-errno returns). See
  ADR-0931 and
  [`docs/architecture/mcp-cgo-direct-migration.md`](../../docs/architecture/mcp-cgo-direct-migration.md)
  for the rollout plan.
