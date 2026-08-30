- **MCP server vmaf binary path resolution post-ADR-0700.**
  `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py:_vmaf_binary()` listed
  `<repo>/libvmaf/build/tools/vmaf` as its second-preference fallback,
  but ADR-0700 moved `libvmaf/` to `core/` last week. When `VMAF_BIN`
  was unset and `/usr/local/bin/vmaf` was absent, the MCP server
  returned a non-existent path and emitted a misleading error pointing
  at the pre-rename location. Updated to `<repo>/core/build/tools/vmaf`
  and synced the three matching path-resolution tests in
  `test_path_and_bench_env.py`. Also corrected
  `scripts/dev/project_modernization_audit.py`'s
  `DEFAULT_SCAN_ROOTS` from `libvmaf/src`, `libvmaf/tools` to
  `core/src`, `core/tools` so the modernization audit actually scans
  the renamed tree.
