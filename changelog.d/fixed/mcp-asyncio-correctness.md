# fix(mcp-server): asyncio correctness — async wrappers + gather return_exceptions + VMAF_MCP_ASYNC guard (ADR-1023)

- `_probe_backends`, `_ffprobe_geometry`, and `_vmaf_version` called
  blocking `subprocess.run` from async coroutines, stalling the event
  loop. Added `_probe_backends_async` and `_ffprobe_geometry_async`
  wrappers via `asyncio.to_thread`; made `_vmaf_version` and
  `_list_backends` async.
- `asyncio.gather` in `_run_vmaf_score_encoded` lacked
  `return_exceptions=True`, silently masking one decode failure when
  two concurrent decodes ran. Added inspection loop that re-raises the
  first exception.
- `VMAF_MCP_ASYNC` env-var accepted arbitrary strings as anyio backend
  names; `"true"` or `"1"` produced a cryptic anyio `RuntimeError`.
  Restricted to well-defined tokens with `"trio"` as the non-asyncio
  canonical.
