<!-- markdownlint-disable MD013 MD060 -->
# ADR-0131: Port Netflix#1382 — `cuMemFreeAsync` → `cuMemFree` in `vmaf_cuda_picture_free`

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: cuda, upstream-port, correctness

## Context

Upstream issue [Netflix#1381][i1381] reports a hard assertion-0 crash
(`Assertion 0 failed`) inside `vmaf_cuda_picture_free()` when two or
more VMAF CUDA sessions free pictures concurrently. Root cause is the
asynchronous free primitive: `cuMemFreeAsync(ptr, stream)` enqueues the
deallocation onto the per-picture stream, which is destroyed two
statements later by `cuStreamDestroy(priv->cuda.str)`. Under
concurrent access the driver observes a stream that no longer owns
the deferred free and fails the internal assertion.

The fork is known-affected. `core/src/cuda/picture_cuda.c:247`
issues exactly the same `cuMemFreeAsync` call, and the fork's
2026-04-18 upstream-backlog audit
([`analysis/upstream-backlog-audit.md`](../../.workingdir2/analysis/upstream-backlog-audit.md))
flagged this as Tier-0 correctness item **T0-1**.

Upstream PR [Netflix#1382][pr1382] (open, 1 commit, +1/-1) switches
the call to the synchronous `cuMemFree`. The fork already issues
`cuStreamSynchronize(priv->cuda.str)` two lines earlier, so the async
variant offered no overlap benefit — `cuMemFree` is both correct and
non-regressive for performance.

## Decision

We port the upstream one-line change to
`core/src/cuda/picture_cuda.c:247`, replacing
`cuMemFreeAsync((CUdeviceptr)pic->data[i], priv->cuda.str)` with
`cuMemFree((CUdeviceptr)pic->data[i])`. The fork's 3-arg
`CHECK_CUDA(cu_f, ...)` macro form and the `(CUdeviceptr)` cast
are preserved — only the call-site changes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `cuMemFreeAsync`, reorder stream destroy after frees | Keeps async-free semantics | Driver still surfaces the bug under multi-context concurrency; reordering alone does not fix it — the `cuStreamSynchronize` on the *same* stream already makes async moot | Does not address the root cause reported in Netflix#1381 |
| Switch to `cuMemFree` (this ADR) | Root-cause fix; upstream-confirmed; synchronous free pairs naturally with the preceding `cuStreamSynchronize` | Loses theoretical async-overlap — but the fork's sync call already gives up that overlap anyway | Chosen |
| Wait for upstream merge, then `/sync-upstream` | No fork-local diff to carry | Upstream PR has been open since 2024-07-15 with no merge signal | Unbounded wait on a confirmed-affecting crash |

## Consequences

- **Positive**: Multi-session CUDA runs no longer hit the assert-0
  path. Removes the Tier-0 correctness blocker. Fork's behaviour now
  matches the upstream-proposed fix exactly, so a future
  `/sync-upstream` sees a clean merge.
- **Negative**: None material. `cuMemFree` is synchronous, but the
  preceding `cuStreamSynchronize` already enforced a barrier — the
  effective runtime cost is unchanged.
- **Neutral / follow-ups**:
  - [`rebase-notes.md`](../rebase-notes.md) updated so a future
    `/sync-upstream` knows this port preceded upstream's merge.
  - Once Netflix merges #1382, the fork's line becomes a trivial
    no-op merge; nothing to undo.

## References

- Upstream issue: [Netflix#1381 — VMAF CUDA `vmaf_cuda_picture_free` Assertion `0` failed][i1381]
- Upstream PR: [Netflix#1382 — Change `cuMemFreeAsync` to `cuMemFree`][pr1382]
- Backlog: [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md) T0-1
- Audit: [`.workingdir2/analysis/upstream-backlog-audit.md`](../../.workingdir2/analysis/upstream-backlog-audit.md) row #1381 / PR #1382
- Source: `req` — user direction to ship Batch-A (T0-1 + T4-4/5/6) as one PR (2026-04-20 popup).

[i1381]: https://github.com/Netflix/vmaf/issues/1381
[pr1382]: https://github.com/Netflix/vmaf/pull/1382
