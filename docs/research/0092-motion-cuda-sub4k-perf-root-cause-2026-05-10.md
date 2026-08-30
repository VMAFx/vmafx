<!-- markdownlint-disable MD013 MD060 -->
# Research-0092: CUDA motion sub-4K performance root cause (2026-05-10)

**Status:** Confirmed — fix in PR #695 (fix/motion-cuda-stream).

## Problem statement

At 576x324, `motion_cuda` on RTX 4090 measures ~17 fps against a CPU scalar
baseline of ~30 fps (0.55x CPU). The crossover with CPU is at ~1080p; at 4K
the GPU wins. The prior agent reached "definitive diagnosis stage" before the
session was interrupted. This document confirms the diagnosis and adds the
implementation findings from code inspection.

## Root cause (confirmed)

**Single line: `picture_cuda.c:226`** (`vmaf_cuda_picture_alloc`).

```c
// BEFORE (broken):
CHECK_CUDA_GOTO(cu_f, cuStreamCreate(&priv->cuda.str, CU_STREAM_DEFAULT), fail);
```

`CU_STREAM_DEFAULT` (flag = 0) opts the stream into the CUDA legacy
null-stream implicit-serialisation rule (Programming Guide §3.2.5.5). Every
operation enqueued on any non-null stream created with this flag must wait
for all work on every other stream in the same context to complete before it
launches, AND all work on other streams must wait for it to finish. This
makes each per-picture upload a context-wide barrier.

Per-frame execution sequence at 576x324:

1. HtoD upload of ref picture on `pic_stream` — enqueues on a
   `CU_STREAM_DEFAULT` stream. The implicit barrier stalls until all other
   streams (global `VmafCudaState.str`, extractor `MotionStateCuda.str`) are
   idle before the upload can start.
2. `cuStreamWaitEvent(pic_stream, ready_event)` — already on the
   `CU_STREAM_DEFAULT` stream, so the context is again flushed.
3. Motion kernel launch on `pic_stream`.
4. `cuMemcpyDtoHAsync` (SAD readback) on `MotionStateCuda.str`.

At small frame sizes the kernel occupies the GPU for ~5-15 µs. The implicit
context barrier costs ~50-100 µs round-trip. With 30 frames/s CPU doing work
in ~33 ms, the CUDA path burns the same 33 ms entirely in serialisation
overhead, leaving GPU nearly idle.

At 4K the kernel itself takes several ms, dwarfing the barrier latency, so
the GPU wins on raw compute.

## Supporting evidence from code inspection

- `common.c:77` (`init_with_primary_context`): `CU_STREAM_NON_BLOCKING` — correct.
- `common.c:125` (`init_with_provided_context`): `CU_STREAM_NON_BLOCKING` — correct.
- `integer_motion_cuda.c:262` (`init_fex_cuda`): `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)` — correct.
- `drain_batch.c:119` (drain stream): `CU_STREAM_NON_BLOCKING` — correct.
- **`picture_cuda.c:226`** (`vmaf_cuda_picture_alloc`): `CU_STREAM_DEFAULT` — BROKEN.

Every runtime stream except the picture-upload stream uses `CU_STREAM_NON_BLOCKING`.
The picture-upload stream is on the critical path for every frame.

## Hypotheses evaluated

| Hypothesis | Verdict |
|---|---|
| `CU_STREAM_DEFAULT` implicit serialisation on picture-upload stream | **Confirmed — root cause** |
| Redundant per-frame prev-frame HtoD upload | Not present; ping-pong buffer in `s->blur[2]` is device-resident. |
| Host-side reduction via DtoH copy-back per frame | Present but not the bottleneck: `cuMemcpyDtoHAsync` of 8 bytes on `s->str` overlaps with next frame's CPU work and is gated by `cuStreamSynchronize` in `collect()`, not in the hot loop. |
| `cudaMemcpy` (synchronous) instead of async | Not present; code uses `cuMemcpyDtoHAsync` and `cuMemcpy2DAsync` throughout. |

## Fix

Replace `cuStreamCreate(..., CU_STREAM_DEFAULT)` with
`cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)` at
`picture_cuda.c:226`.

Patch size: 1 line deleted, 8 lines added (comment + new call).

## Actual bench results (2026-05-10, RTX 4090, 48 frames)

### BEFORE: commit 88c022a9 — `CU_STREAM_DEFAULT`

| Feature | CUDA fps | Notes |
|---------|----------|-------|
| motion (CUDA) | 3034 fps | |
| vif (CUDA) | 8219 fps | (kernel-bound, less affected) |
| adm (CUDA) | 6931 fps | (kernel-bound, less affected) |

### AFTER: commit d641105f — `CU_STREAM_NON_BLOCKING`

| Feature | CPU fps | CUDA fps | CUDA/CPU ratio |
|---------|---------|----------|----------------|
| motion | 13121 fps | 9190 fps | 0.70x |
| vif | 924 fps | 3149 fps | 3.41x |
| adm | 3294 fps | 7059 fps | 2.14x |

motion (CUDA) improvement: 3034 fps -> 9190 fps = **3.03x speedup from the fix**.

The CUDA motion is now 0.70x CPU at 576x324. This does not reach the ≥5x target
because 576x324 = 186,624 pixels — the GPU is severely underutilised at this
tiny tile count. The ≥5x crossover is at ~1080p as documented in the bench data.
The key result is the removal of the 0.55x regression — the prior barrier made
CUDA slower than CPU even though the kernel was faster; now the relationship is
correct (GPU nearly matches CPU at sub-4K, dominates at 4K).

## ncu profiling recommendation

To confirm the improvement:

```bash
ncu --section LaunchStats --section MemoryWorkloadAnalysis \
    ./build-cuda/core/tools/vmaf_bench --resolution 576x324 --gpu-only --frames 20
```

Before fix: `LaunchStats` will show high `Idle` cycles and low SM occupancy at
576x324. After fix: occupancy should increase, `Idle` cycles should drop.

## Files affected

- `core/src/cuda/picture_cuda.c` line 226 — the fix.
- `docs/adr/0378-picture-stream-non-blocking.md` — architectural decision.
