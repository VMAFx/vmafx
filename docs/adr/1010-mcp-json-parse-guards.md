<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1010: MCP server JSON parse guards — vmaf output and ffprobe output

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `mcp`, `python`, `error-handling`, `correctness`

## Context

Two bare `json.loads` calls in `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` have
no `JSONDecodeError` guard:

1. **`server.py:432`** (`_run_vmaf_score`) — `json.loads(output.read_text(...))`:
   If vmaf exits 0 but writes a partial or empty JSON file (e.g. disk-full / OOM
   kill mid-write), the uncaught `JSONDecodeError` propagates as an untyped
   exception.  MCP clients receive a generic server-error frame with no actionable
   message.

2. **`server.py:1716`** (`_ffprobe_geometry`) — `json.loads(result.stdout)`:
   `ffprobe` returns `rc=0` with empty or truncated stdout for some container
   formats that have no video track (empty file, audio-only, corrupt header).
   The bare `json.loads` then raises `JSONDecodeError` inside
   `vmaf_score_encoded`, crashing the tool handler with no diagnostic.

Note: several other `json.loads` calls in the file already have `JSONDecodeError`
guards added by prior audits (lines 1056, 1213, 1289, 1290, 1364, 1366).

Also note: the `asyncio.current_task().get_name()` crash (r3-integration-boundaries
finding), duplicate `_run_benchmark` (r4-deprecated-api), and stale `libvmaf/`
path references were all fixed in PR #537 (ADR-0774).  This ADR closes the two
remaining `r3-config-parsing` findings.

## Decision

Wrap both `json.loads` calls in `try/except json.JSONDecodeError as exc: raise
RuntimeError(…) from exc` with a diagnostic message that names the likely cause
(disk-full / OOM kill for vmaf, no-video-track for ffprobe).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Let JSONDecodeError propagate | Minimal diff | No actionable message; confuses callers | Rejected |
| Return None on parse failure | Avoids raising | Silently suppresses the error | Rejected |
| Wrap + reraise as RuntimeError | Clear message, consistent with other handlers | Slightly more verbose | Chosen |

## Consequences

- **Positive**: MCP clients receive a clear error message instead of a raw
  `JSONDecodeError` traceback when vmaf or ffprobe produce invalid JSON.
- **Neutral**: No change to successful-path behaviour.
- **Negative**: None.

## References

- r3/r4 audit findings: `[r3-config-parsing]` cluster
- ADR-0774 (MCP server audit round 26, prior fixes)
