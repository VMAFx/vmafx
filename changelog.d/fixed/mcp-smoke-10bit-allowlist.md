- `test_mcp_smoke`'s `compute_vmaf` 10-bit case passes again. The case writes
  its `yuv420p10le` fixtures to `/tmp` at run time, but the C-MCP path
  allowlist added in PR #1054 canonicalises every caller-supplied YUV path and
  admits only `<repo>/testdata`, `<repo>/model`, `<repo>/python/test/resource`,
  `/workspace/python/test/resource` and `$VMAF_MCP_ALLOW`. `/tmp` is
  deliberately not a default root, so `score_yuv_pair()` returned `-EACCES`,
  the response carried no `score` field, and the assertion tripped — the single
  red test in the "MCP Smoke (Embedded C + Python Server)" CI job. The case now
  extends the allow-set through the documented `VMAF_MCP_ALLOW` escape hatch
  for its own duration and `unsetenv()`s it afterwards, so the remaining cases
  still run against the default roots. The allowlist itself is unchanged and
  its rejection behaviour remains covered by
  `core/test/test_mcp_compute_vmaf_allowlist.c`.
