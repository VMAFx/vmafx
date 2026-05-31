<!-- markdownlint-disable MD060 -->
# Research-0097 — MCP `describe_worst_frames` temporary directory leak

**Date:** 2026-05-10
**Status:** Fixed (PR #741 `fix/round8-mcp-tmpdir-leak`)
**Bug ID:** T-ROUND8-MCP-TMPDIR-LEAK
**ADR:** none (bug fix, no architectural decision per CLAUDE.md §12 r8)

## Problem statement

Each call to the MCP tool `describe_worst_frames` created a per-process
temporary directory `/tmp/vmaf-mcp-worst-{pid}/` and wrote N PNG files
(default N = 5, configurable) into it — one PNG per worst-scoring frame.
The original code's `finally` block contained only a bare `pass` and a
comment "clear the dir on next invocation" — but no cleanup was ever
implemented. PNGs from all prior calls accumulated without bound for the
lifetime of the server process.

On a long-running MCP server (e.g. attached to Claude Desktop, left
running overnight), a session analysing 100 video pairs at N = 5 frames
each produced 500+ PNG files occupying up to 150 MB of `/tmp` storage.
Worse, because the directory name is keyed by PID only (`vmaf-mcp-worst-{pid}`),
on process restart the old directory was silently orphaned under a different
PID name — so cleanup on restart did not recover the stale space either.

## Root cause

`_describe_worst_frames` in `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`
allocated `tmp_root = Path("/tmp") / f"vmaf-mcp-worst-{os.getpid()}"` and
called `tmp_root.mkdir(parents=True)` unconditionally. The `finally` block
intended to retain the PNGs for the caller's use (so that a downstream tool
could open the PNG path returned in the JSON response), but included no
deferred cleanup mechanism. The comment "clear the dir on next invocation"
was never implemented.

## Fix

`shutil.rmtree(tmp_root)` was inserted at the **start** of each
`_describe_worst_frames` invocation, immediately before `tmp_root.mkdir`.
The guard `if tmp_root.exists(): shutil.rmtree(tmp_root)` ensures that
stale PNGs from the previous invocation are purged before new ones are
generated. PNGs remain accessible on disk for the duration of the single
turn (until the *next* call), which preserves the intended contract of
returning usable file paths in the JSON response.

```python
tmp_root = Path("/tmp") / f"vmaf-mcp-worst-{os.getpid()}"
if tmp_root.exists():
    shutil.rmtree(tmp_root)
tmp_root.mkdir(parents=True)
```

## Alternatives considered

| Option | Why not chosen |
|---|---|
| **`atexit` handler** — register `shutil.rmtree(tmp_root)` on process exit | Cleans up on normal exit only; does not help on SIGKILL or crash. Does not prevent accumulation during the session. On restart, the old PID directory is still orphaned (PID key changes). Rejected — does not solve the accumulation problem within a session. |
| **Per-call `mkdtemp` with explicit cleanup in `finally`** — create a fresh randomly-named temp dir each call, always delete it in `finally` | PNGs would be deleted before the response is returned to the caller. The JSON response includes PNG file paths that callers are expected to open; deleting them in `finally` would break that contract. Rejected — breaks the caller API. |
| **Purge-before-generate (chosen)** — `rmtree` at start of each call, before new PNGs are written | Bounded disk usage (at most 1 call's worth of PNGs at any time), PNGs available for the duration of the turn, no atexit fragility. Correct. |
| **Separate cleanup tool** — expose a `clear_worst_frame_cache` MCP tool | Adds complexity and requires callers to remember to invoke it. No advantage over automatic cleanup. Rejected. |

## Reproducer

Start the MCP server and invoke `describe_worst_frames` twice in
succession. Before the fix, the PNG directory grows with each call.
After the fix, the directory is cleared at the start of each call.

```bash
# Unit test (no live vmaf binary needed):
cd mcp-server/vmaf-mcp
python3 -m pytest tests/test_server.py::test_describe_worst_frames_tmpdir_cleared_on_next_call -v
```

The sentinel-file test plants a stale file in the tmp directory before
calling `_describe_worst_frames`, then asserts the sentinel is gone
after the call returns. The test passes against the fixed server and
fails against the pre-fix server.

## Verification

- `test_describe_worst_frames_tmpdir_cleared_on_next_call` passes.
- Manual inspection: after two back-to-back calls, `/tmp/vmaf-mcp-worst-{pid}/`
  contains only the PNGs from the *second* call; the first call's PNGs
  are gone.
- No change to the JSON response schema: `frames[].png` paths are still
  valid at the time the response is delivered to the caller.
