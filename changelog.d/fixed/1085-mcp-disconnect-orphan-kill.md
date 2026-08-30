- MCP: kill vmaf/vmaf-tune/python child processes on client disconnect.
  Go `runVmafScore` and `delegateToPythonEval` switched from bare
  `exec.Command` to `exec.CommandContext`; Python `_communicate_with_timeout`
  now catches `asyncio.CancelledError` and kills the child before re-raising,
  symmetric with the existing `TimeoutError` path (ADR-1085).
