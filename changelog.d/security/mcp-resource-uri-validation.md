- **MCP direct-cgo path: allowlist bypass in `resolveModelArgToPath` fixed** —
  `cmd/vmafx-mcp/impl_direct.go::resolveModelArgToPath` returned absolute paths
  supplied via `model: "path=/arbitrary/path"` or a bare `/absolute/path` argument
  without passing them through `libvmaf.ValidatePath`.  This allowed an MCP client
  with `VMAFX_MCP_DIRECT=1` enabled to coerce `vmaf_score` into opening any file the
  process can read, bypassing the `VMAF_MCP_ALLOW`-controlled allowlist.  Fixed by
  routing every resolved path (absolute, relative, and bare-stem candidates) through
  `libvmaf.ValidatePath` before returning.  Only affects operators who have
  `VMAFX_MCP_DIRECT=1` set; the default subprocess path was already protected.
  Regression test: `TestResolveModelArgToPath_AllowlistEnforced`.
