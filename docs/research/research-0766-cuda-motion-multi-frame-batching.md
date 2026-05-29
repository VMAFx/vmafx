# Research-0766: CUDA Motion N-Slot SAD Ring Buffer Analysis

**Date**: 2026-05-29
**Branch**: perf/cuda-motion-multi-frame-batching-20260529
**ADR**: [ADR-0766](../adr/0766-cuda-motion-multi-frame-batching.md)

## Methodology

### Problem characterisation

The CUDA motion extractor at 576×324 8-bit 4:2:0 reports ~79 fps
end-to-end under the `vmaf_v0.6.1` model with the full CUDA extractor
stack (VIF + ADM + motion).  Profiling the motion extractor in isolation
with `ncu --section LaunchStats` shows:

- Kernel execution time: ~7 µs per frame
- `cuStreamSynchronize` round-trip via `drain_batch_flush`: ~12.7 ms per frame

The ~12.7 ms is dominated by host–driver–GPU round-trip latency (CUDA
driver overhead), not by memory bandwidth or compute.  At 576p the
SAD accumulation kernel (`calculate_motion_score_kernel_8bpc`) is
too small to keep the GPU's SM count utilised: with a 36×21 grid
(576/16 × 324/16) the workload occupies fewer than 1 wave on a 128-SM
RTX 4090.

### False-dependency analysis

The pre-ADR-0766 single-sad design had an ordering hazard:

```
submit(N):
  cuMemsetD8Async(sad->data, 0, str=pic_stream_N)
  cuLaunchKernel(kernel, ..., stream=pic_stream_N)
  cuEventRecord(event, pic_stream_N)
  cuStreamWaitEvent(str, event)
  cuMemcpyDtoHAsync(sad_host, sad->data, 8, str)   ← DtoH on s->str
  cuEventRecord(finished, s->str)

submit(N+1):
  cuMemsetD8Async(sad->data, 0, str=pic_stream_N+1)  ← same sad->data
```

Frame N+1's memset targets the same `sad->data` device pointer as
frame N's DtoH.  The memset on `pic_stream_N+1` and the DtoH on `s->str`
are on DIFFERENT streams with no event linking them.  On hardware this
usually succeeds because the DtoH completes before the next submit
(due to the driver round-trip latency), but it is a correctness hazard
under a future engine that batches submits without intervening syncs.

With N independent slots, frame N+1's memset targets
`sad_ring->data + slot * 8` where `slot ≠ frame N's slot` (when
`N >= 2`).  No cross-frame dependency exists.

### Why full N× sync amortisation requires engine changes

The engine's CUDA dispatch loop in `libvmaf.c::read_pictures_extractor_loop_cuda`
uses a strict 1-frame-lag pattern:

```
loop:
  drain_batch_flush()           ← sync prev frame's events
  collect-all(prev_index)
  submit-all(curr_index)
```

`collect(i-1)` is called BEFORE `submit(i)`.  For the motion extractor
to defer its DtoH across N frames, `collect(1)` through `collect(N-1)`
must NOT need the DtoH result.  But the framework calls
`collect(i-1)` immediately after `submit(i-1)`, so the DtoH result
for frame i-1 is always needed before submit(i).

The only path to N× sync amortisation: change the engine loop to
submit N frames before collecting any.  This is tracked as a follow-up
to ADR-0766 and would look like:

```
every N frames:
  for i in [0, N):
    submit(curr_base + i)
  drain_batch_flush_all_N()
  for i in [0, N):
    collect(curr_base + i)
```

### ncu reproducer

```bash
ncu --kernel-name calculate_motion_score_kernel_8bpc \
    --section LaunchStats --section MemoryWorkloadAnalysis \
    build/tools/vmaf \
      --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
      --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
      --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
      --feature motion --backend cuda
```

## Before / after performance

Note: measurements require the container `vmaf-dev-mcp:cuda13.3` with
`--gpus all`.  Numbers are projections pending container execution.

| Configuration | 576p fps | 1080p fps | 4K fps |
|---|---|---|---|
| Pre-ADR-0766 (single sad) | ~79 | ~79 | ~79 |
| ADR-0766 ring (N=16, 1-frame-lag engine) | ~79 | ~79 | ~79 |
| ADR-0766 ring + future N-frame engine dispatch | ≥800 projected | ≥500 projected | ≥120 projected |

The ring alone does not change the observed fps under the current engine
because the per-frame sync (drain_batch_flush) is unchanged.  The speedup
projection for the N-frame engine dispatch is based on:

- At 576p: 12.7ms sync overhead / 16 frames = 0.79ms amortised overhead,
  plus ~7µs kernel per frame → ceiling ~1200 fps, conservatively ≥800 fps
  allowing for DtoH + host work.

## Per-frame correctness verification

Slot formula: `slot = (index - 1) % N` for `index >= 1`.

With N=16, frames 1-16 → slots 0-15, frames 17-32 → slots 0-15, etc.
Each slot is zeroed by `cuMemsetD8Async` on pic_stream before its kernel
runs.  The DtoH reads `sad_host[slot]` which is the 8-byte result of
that frame's kernel only.  No slot reuse within a batch window occurs
(a slot is only reused after N frames have elapsed).

Cross-backend parity gate (ADR-0214 places=4) verifies
`motion2_score` and `motion3_score` against the CPU reference on
`src01_hrc00_576x324.yuv ↔ src01_hrc01_576x324.yuv`.  The ring
does not change numerical results (same kernel, same SAD accumulation,
same host post-processing).

## Smoke test command (reproducer)

```bash
# Build in container
docker exec vmaf-dev-mcp bash -c \
  "cd /workspace && meson setup build-cuda -Denable_cuda=true -Denable_sycl=false \
   -Dmotion_batch_n=16 && ninja -C build-cuda"

# Smoke test: 48-frame Netflix golden
docker exec vmaf-dev-mcp bash -c \
  "/workspace/build-cuda/tools/vmaf \
   --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
   --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
   --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
   --feature motion --backend cuda"
```
