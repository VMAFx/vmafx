<!-- markdownlint-disable MD060 -->
# ADR-0378: Per-picture CUDA streams must use CU_STREAM_NON_BLOCKING

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, cuda-reviewer agent
- **Tags**: cuda, performance, gpu, feature-extractor, fork-local

## Context

Each device-side `VmafPicture` created by `vmaf_cuda_picture_alloc` owns a
private `CUstream` used for asynchronous HtoD uploads and related work.
Before this ADR that stream was created with `cuStreamCreate(...,
CU_STREAM_DEFAULT)`.

`CU_STREAM_DEFAULT` is the legacy default-stream flag. Per CUDA programming
guide §3.2.5.5, any stream created with this flag (or via `cuStreamCreate`
with flag `0`) participates in the implicit NULL-stream serialisation rule:
operations enqueued on a non-NULL default-flagged stream wait for all work
on every *other* stream in the same CUDA context to complete before
launching, and all work on other streams waits for those operations to
complete. This means every per-frame picture upload effectively acts as a
full context-scope barrier.

At sub-4K resolutions (e.g. 576x324) the kernel occupies the GPU for only a
few microseconds. The implicit synchronisation overhead from
`CU_STREAM_DEFAULT` dominates and produces measured throughput of ~17 fps on
an RTX 4090 — 0.55x the CPU scalar baseline of ~30 fps — rather than the
expected ≥5x speedup.

The global CUDA state stream (`VmafCudaState.str`, created in `common.c`)
and the per-extractor compute stream (`MotionStateCuda.str`, created in
`integer_motion_cuda.c:init_fex_cuda`) both already use
`CU_STREAM_NON_BLOCKING`. The picture-upload stream was the single outlier.

## Decision

We will replace `cuStreamCreate(&priv->cuda.str, CU_STREAM_DEFAULT)` with
`cuStreamCreateWithPriority(&priv->cuda.str, CU_STREAM_NON_BLOCKING, 0)` in
`vmaf_cuda_picture_alloc` (`core/src/cuda/picture_cuda.c`).

All three CUDA stream-creation sites in the runtime now consistently use
`CU_STREAM_NON_BLOCKING`. No other code changes are required: the picture
stream's relationship to the extractor stream is already managed by explicit
`cuStreamWaitEvent` / `cuEventRecord` pairs in every extractor's `submit`
path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `CU_STREAM_DEFAULT`, add explicit `cuStreamSynchronize` calls | No stream-creation change | Every submit still serialises the entire context; the synchronise just makes it explicit. No performance gain. | Does not fix the root cause. |
| Use `cuStreamCreate` with flag `CU_STREAM_NON_BLOCKING` (no priority) | Removes implicit barrier | `cuStreamCreate` is deprecated in favour of `cuStreamCreateWithPriority`; mixing the two APIs in the same file adds inconsistency. | Functional but stylistically inferior to using `cuStreamCreateWithPriority` at priority 0. |
| Per-frame stream creation and destruction | Allows async teardown | Per-frame `cuStreamCreate`+`cuStreamDestroy` cost ~5-10 µs/call — several times the kernel runtime at 576x324. Would make things worse. | Not viable for sub-4K benchmarking. |

## Consequences

- **Positive**: Removes the implicit per-frame context barrier on the
  picture-upload stream. At 576x324 motion, expected throughput improvement
  from ~17 fps to the ≥5x CPU baseline target.
- **Positive**: All three runtime stream-creation sites now consistently use
  `CU_STREAM_NON_BLOCKING`, making the codebase more uniform and the
  implicit-barrier hazard impossible to reintroduce by copy-paste from this
  file.
- **Negative**: None. The event-pair inter-stream synchronisation (`cuStreamWaitEvent`
  in every extractor's submit path) already correctly handles ordering between
  the upload stream and the compute stream; removing the implicit barrier does
  not introduce any new race condition.
- **Neutral / follow-ups**: Profile with `ncu --section LaunchStats` at
  576x324 after this change to confirm that stream-launch overhead is no
  longer the bottleneck and that GPU utilisation has improved. Suggest
  `--section MemoryWorkloadAnalysis` to confirm upload bandwidth is not a
  secondary bottleneck.

## References

- CUDA Programming Guide §3.2.5.5 "Implicit Synchronization" (default-stream
  behaviour).
- NVIDIA Developer Blog "GPU Pro Tip: CUDA 7 Streams Simplify Concurrency"
  (background on non-blocking streams).
- `core/src/cuda/common.c` — `init_with_primary_context` / `init_with_provided_context`
  already use `CU_STREAM_NON_BLOCKING`; this ADR aligns `picture_cuda.c`.
- `core/src/feature/cuda/integer_motion_cuda.c:262` — extractor stream
  correctly uses `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)`.
- `docs/research/0092-motion-cuda-sub4k-perf-root-cause-2026-05-10.md` — perf
  benchmark data and root-cause analysis.
- Related: `docs/research/0092-perf-bench-multi-backend-2026-05-10.md` (PR #694
  bench data).
- req: "Re-investigate why the motion feature on CUDA (RTX 4090) is slower
  than CPU scalar at sub-4K."
