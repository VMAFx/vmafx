<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1085: MCP streaming backpressure — kill child processes on client disconnect

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `mcp`, `security`, `go`, `python`

## Context

The MCP server exposes long-running tools (`vmaf_score`, `run_benchmark`,
`run_compare`, `run_ladder`, `run_tune_per_shot`, `describe_worst_frames`,
`eval_model_on_split`) that delegate work to child processes (vmaf CLI,
vmaf-tune CLI, Python evaluation script, ffmpeg).

Two separate bugs caused orphan child processes when an MCP client disconnected
mid-stream:

**Bug 1 — Go `runVmafScore` and `delegateToPythonEval` ignored the request
context.**  Both functions received a `ctx context.Context` from the MCP
dispatch layer but launched child processes via `exec.Command(...)` instead of
`exec.CommandContext(ctx, ...)`.  A client disconnecting during a long vmaf or
onnxruntime evaluation left the subprocess running to completion on CPU or GPU.

**Bug 2 — Python `_communicate_with_timeout` did not handle
`asyncio.CancelledError`.**  The function handled `asyncio.TimeoutError`
correctly (killed the child), but if the MCP framework cancelled the outer
coroutine (the standard mechanism for a client disconnect on the stdio or HTTP
transport), `asyncio.wait_for` raised `CancelledError` which propagated upward
without killing the child process.  The child ran to completion as an orphan
consuming CPU, GPU memory, and open file descriptors.

## Decision

1. **Go**: thread the caller's `ctx context.Context` through `runVmafScore`,
   `runVmafScoreDirect`, and `delegateToPythonEval`; change all internal
   `exec.Command(...)` calls in those functions to `exec.CommandContext(ctx, ...)`
   so that OS-level SIGKILL is delivered to the child when the context is
   cancelled.

2. **Python**: add an `except asyncio.CancelledError` branch to
   `_communicate_with_timeout` that kills the child process and then re-raises,
   mirroring the existing `asyncio.TimeoutError` handling.  Re-raising is
   mandatory so asyncio can propagate the cancellation to the MCP framework.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `asyncio.shield` on each `proc.communicate()` call | Prevents cancellation mid-read, simpler error semantics | Orphan process stays alive indefinitely regardless of client state | The whole point is to kill the orphan |
| Per-tool cancel wrappers instead of fixing `_communicate_with_timeout` | More granular | Code duplication across 10+ call sites | Central fix in the shared helper is cleaner and covers all tools atomically |
| Leave as-is, rely on OS to reap orphans at server shutdown | No code change | Orphan vmaf/ffmpeg processes consume GPU VRAM for the full lifetime of the server (hours in container deployments) | Unacceptable in long-lived container deployments |

## Consequences

- **Positive**: client disconnect during any long-running MCP tool call now
  terminates the underlying child process promptly, freeing CPU, GPU, and file
  resources.
- **Positive**: symmetric cancel handling across Go and Python implementations.
- **Negative**: none. The change is purely additive — contexts that are never
  cancelled behave identically to before.
- **Neutral / follow-ups**: `runVmafScoreDirect` now also receives a context and
  passes it to `libvmaf.ScoreDirect`, which already accepted one.  The old
  `context.Background()` comment citing "future ADR" is retired by this ADR.

## References

- ADR-1018 (`r5-mcp-streaming` GPU probe CommandContext finding, same class of bug).
- `cmd/vmafx-mcp/impl.go` — `runVmafScore` (line 224), `delegateToPythonEval` (line 530).
- `cmd/vmafx-mcp/impl_direct.go` — `runVmafScoreDirect` (line 83 context.Background() removed).
- `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` — `_communicate_with_timeout`.
- Source: automated workflow audit (mcp-streaming-backpressure).
