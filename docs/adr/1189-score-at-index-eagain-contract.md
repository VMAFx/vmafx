# ADR-1189: `*_score_at_index` reports in-flight frames as `-EAGAIN`

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: api, cuda, gpu, contract

## Context

The GPU dispatch path is double-buffered. `read_pictures_extractor_loop`
submits frame N's kernels and only collects them while processing frame N+1
(or at the terminal `vmaf_read_pictures(NULL, NULL)`). Between those two points
the extractor's result for index N is not in the feature collector and, on
CUDA, the drain batch that fences the DtoH readback has not been flushed.

The three public per-index readers —
`vmaf_feature_score_at_index`, `vmaf_score_at_index` and
`vmaf_score_at_index_model_collection` — read the collector with no such fence
and no in-flight check. Reading is not a data race:
`vmaf_feature_collector_get_score` takes the collector's mutex. It is a *stale*
read, and the model paths make it worse than a missing value: they fall through
to `vmaf_predict_score_at_index`, which predicts from whatever subset of the
frame's features happens to be written. The caller gets a plausible-looking
number for a frame that has not finished. This is the upstream half of
`T-UPSTREAM-1305` (Netflix/vmaf#1305), whose reported symptom is intermittent
wrong scores and NaNs.

The header already documents `-EAGAIN` as "not available yet" for retroactively
written features ([ADR-1073](1073-mcp-score-at-index-eagain-guard.md) is about that
guard), so the vocabulary for "come back later" exists on this surface.

## Decision

We add an in-flight guard to all three per-index readers: when any registered
feature-extractor context still has un-collected asynchronous work for exactly
the requested index (`gpu_pending && gpu_pending_index == index`), the call
returns `-EAGAIN` instead of reading the collector. Callers flush with
`vmaf_read_pictures(NULL, NULL)` (or advance far enough that the double buffer
has rotated) and retry. Indices other than the single in-flight one are
unaffected — each extractor context tracks exactly one pending submission — so
the pooling helpers, the CLI and the in-tree tests, which all read after the
terminal flush, see no behaviour change.

We deliberately do **not** make the readers themselves fence.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `-EAGAIN` contract (chosen) | Cannot deadlock, cannot mutate engine state, cannot change any score; a caller that was previously getting a silently-wrong number now gets a retryable error | Callers polling mid-stream must handle a retry | Smallest change that removes the wrong answer |
| Full fence inside the readers (drain-batch flush + `cuCtxSynchronize` + per-extractor `collect()`) | The value is simply available | A getter would have to run `collect()`, which writes the collector and clears `gpu_pending` — the next frame's Phase-1 collect would then double-write the same index and fail with `-EINVAL`. It also reorders GPU work from a call the API documents as a read | Turns a read into a hidden state machine step with a known double-collect failure |
| `vmaf_thread_pool_wait` in the readers | Fences CPU extractors too | Metadata callbacks (`VmafMetadataConfiguration`) are invoked from inside pool workers, so a callback that calls a reader would wait on its own pool and deadlock | Introduces a deadlock where there was only staleness; the collector lock already makes the CPU read safe |
| Document the hazard and change nothing | Zero risk | Leaves a public entry point returning fabricated predictions | The ledger's closure condition requires a fence or a contract |

## Consequences

- **Positive**: a mid-stream read of an in-flight frame is now a loud,
  retryable `-EAGAIN` instead of a prediction over a half-written feature row.
  No score moves; the Netflix golden gate is untouched (it reads after the
  terminal flush).
- **Negative**: a caller that polled `vmaf_score_at_index` while frames were in
  flight and tolerated whatever came back now sees `-EAGAIN`. That is a
  behaviour change on a public entry point, which is why it has its own ADR and
  a documented contract in [`docs/api/index.md`](../api/index.md).
- **Neutral / follow-ups**: the guard is O(number of registered extractors) and
  runs once per call — negligible against the collector lock it precedes.
  CPU-only builds keep `gpu_pending` false throughout, so the guard is inert
  there.

## References

- `T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03` in
  [`docs/state.md`](../state.md).
- Netflix/vmaf#1305.
- [ADR-1073](1073-mcp-score-at-index-eagain-guard.md) — the prior `-EAGAIN` change
  on this surface (model-score short-circuit), whose vocabulary this reuses.
- [ADR-1187](1187-cuda-drain-batch-state-owned.md) — the fork-original half of
  the same ledger row.
