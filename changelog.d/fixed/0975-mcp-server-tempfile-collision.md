# fix(mcp-server): use NamedTemporaryFile to eliminate task-name collision risk

`_run_vmaf_score` previously derived its output temp path from
`asyncio.current_task().get_name()`, which is not guaranteed unique under
high concurrency or when tasks are renamed. Replaced with
`tempfile.NamedTemporaryFile(delete=False, suffix=".json")` which delegates
uniqueness to the OS. The `finally` block cleanup path (`output.unlink`) is
unchanged. (Round 26 audit A.2; ADR-0975)
