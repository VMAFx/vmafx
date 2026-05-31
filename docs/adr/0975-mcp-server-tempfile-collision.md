<!-- markdownlint-disable MD013 MD060 -->
# ADR-0975: Use NamedTemporaryFile in _run_vmaf_score to eliminate task-name collision risk

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `mcp`, `security`, `concurrency`

## Context

`_run_vmaf_score` in `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` derived its
output JSON path from `asyncio.current_task().get_name()`. Task names in asyncio
are not unique — they are reassigned by the runtime (e.g. `Task-1`, `Task-2`, ...)
and can be overridden by callers. Under concurrent load, two tasks with the same
name would map to the same path, causing one task to overwrite the other's output
or read stale data. Additionally, the OS `/tmp` namespace has wide permissions, so
a predictable pattern like `vmaf-mcp-<pid>-Task-1.json` is trivially guessable by
other processes on the same host (temp-file race). This was flagged as Round 26
audit finding A.2.

## Decision

Replace the manual path construction with `tempfile.NamedTemporaryFile(delete=False,
suffix=".json")`. The OS-provided unique name eliminates both the collision risk and
the predictability issue. Ownership of the file is passed to the existing `finally`
block which calls `output.unlink(missing_ok=True)`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `uuid.uuid4()` suffix | Simple | Still under `/tmp` with guessable prefix | Less idiomatic; stdlib `tempfile` is the standard answer |
| `tempfile.mkstemp()` | OS-unique | Slightly more verbose | Equivalent; `NamedTemporaryFile(delete=False)` is cleaner |
| `tempfile.TemporaryDirectory` per call | Isolates file entirely | Overhead of directory creation per score call | Unnecessary isolation for a single JSON file |

## Consequences

- **Positive**: concurrent `_run_vmaf_score` calls are safe under arbitrary task
  renaming; the output path cannot be predicted or collided by external processes.
- **Negative**: none. The `finally`-block cleanup contract is unchanged.
- **Neutral / follow-ups**: two regression tests added (`test_score_tempfile_uses_unique_path`,
  `test_score_tempfile_cleaned_up_on_exception`).

## References

- Round 26 audit finding A.2 (user request 2026-05-31).
- `tempfile` stdlib docs: <https://docs.python.org/3/library/tempfile.html>
