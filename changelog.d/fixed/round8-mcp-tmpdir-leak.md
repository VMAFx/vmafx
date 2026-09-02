- **`describe_worst_frames` MCP tool accumulated PNG files in
  `/tmp/vmaf-mcp-worst-{pid}/` indefinitely across repeated calls.**
  The docstring promised to clear the directory on the next invocation,
  but the `finally` block was a bare `pass` with no cleanup code. On a
  long-running MCP server that handles many `describe_worst_frames`
  requests, PNG files from all prior calls accumulated without bound
  (up to ~5–10 MB per frame, 32 frames per call). The fix adds a
  `shutil.rmtree(tmp_root)` call at the start of each invocation,
  before any new PNGs are generated, so at most one call's worth of
  PNGs occupies disk at any time. PNGs remain accessible for the caller
  between the response being returned and the next invocation. A
  regression test (`test_describe_worst_frames_tmpdir_cleared_on_next_call`)
  plants a sentinel file and verifies it is absent after the subsequent
  call. Surfaced by round-8 bug-hunt, angle 1 — MCP server resource-leak
  audit (T-ROUND8-MCP-TMPDIR-LEAK).
