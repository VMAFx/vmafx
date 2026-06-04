<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1023: MCP server asyncio correctness — async wrappers for blocking I/O

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `mcp`, `asyncio`, `python`, `correctness`

## Context

The MCP server (`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`) runs on a
single-threaded asyncio event loop. Several functions that run blocking
`subprocess.run` calls were invoked directly from async coroutines, stalling
the event loop for the duration of the subprocess. The affected sites were:

1. `_probe_backends(vmaf)` — called from `_run_vmaf_score`, `_probe_backend`,
   and `_list_backends` (all async), runs `vmaf --help` via `subprocess.run`.
2. `_ffprobe_geometry(path)` — called from `_run_vmaf_score_encoded` (async),
   runs `ffprobe` via `subprocess.run`.
3. `_vmaf_version()` — called from the async `_call_tool` dispatch handler,
   runs `vmaf --version` and `vmaf --help` via `subprocess.run`.
4. `asyncio.gather(...)` in `_run_vmaf_score_encoded` lacked
   `return_exceptions=True`, so a failure in one decode task would silently
   cancel the other without surfacing a clear error to the caller.
5. `VMAF_MCP_ASYNC` env-var parsing in `main()` accepted arbitrary strings as
   anyio backend names, causing a confusing `RuntimeError` from anyio on
   ambiguous values such as `"true"` or `"1"`.

## Decision

We will:

1. Add `_probe_backends_async(vmaf)` — an async wrapper that returns the
   cached result on a cache hit and delegates to
   `asyncio.to_thread(_probe_backends, vmaf)` on a miss. All async
   call sites use the async wrapper.

2. Add `_ffprobe_geometry_async(path)` — an async wrapper that delegates
   entirely to `asyncio.to_thread(_ffprobe_geometry, path)`.

3. Make `_vmaf_version()` and `_list_backends()` async, using
   `asyncio.to_thread` for the blocking `subprocess.run` calls and
   `_probe_backends_async` for the help probe.

4. Add `return_exceptions=True` to the `asyncio.gather` call in
   `_run_vmaf_score_encoded` and inspect results to re-raise the first
   exception.

5. Restrict `VMAF_MCP_ASYNC` to well-defined tokens: `""` / `"asyncio"` /
   `"0"` / `"false"` / `"no"` → `asyncio.run`; `"1"` / `"true"` /
   `"yes"` / `"trio"` → `anyio.run(backend="trio")`; any other value is
   treated as an explicit anyio backend name (e.g. `"uvloop"`).

The synchronous `_probe_backends` and `_ffprobe_geometry` functions are
preserved unchanged so sync call sites (test helpers, offline tooling)
continue to work.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rewrite `_probe_backends` as natively async | Single implementation | Breaks sync callers; needs `asyncio.get_event_loop()` fallback | Unnecessary churn given the thin wrapper pattern |
| Use `loop.run_in_executor` directly | Standard library, no helper | More verbose; `asyncio.to_thread` is idiomatic Python 3.9+ | No advantage over `asyncio.to_thread` |
| NOLINT the blocking sites | Quick | Hides a real bug class | Not a fix |

## Consequences

- **Positive**: The event loop is never blocked by an external process.
  Concurrent MCP tool calls no longer serialise on the subprocess wait.
- **Positive**: `asyncio.gather` failure mode is now explicit: a decode
  error for one input raises immediately with a clear message rather than
  being masked.
- **Positive**: `VMAF_MCP_ASYNC` rejects ambiguous values at startup rather
  than producing a cryptic anyio error.
- **Negative**: Marginal complexity increase from the thin async wrapper
  functions.
- **Neutral**: The `_probe_backends` cache means the thread-hop cost is
  paid at most once per binary path per process lifetime.

## References

- Reported as part of the r5-python-async + r5-integration-boundaries
  review round.
- Related: ADR-0608 (MCP tool surface), ADR-0988 (JSON serialisation).
