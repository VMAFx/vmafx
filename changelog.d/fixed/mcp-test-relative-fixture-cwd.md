- **MCP parameter-rejection tests no longer depend on the invocation directory.**
  `test_vmaf_score_rejects_invalid_core_params` passed repository-relative fixture
  paths, which resolve against the process CWD, so run from `mcp-server/vmaf-mcp/`
  all three cases failed on the path allowlist instead of reaching the parameter
  checks they name. The paths are now anchored to the repository root.
