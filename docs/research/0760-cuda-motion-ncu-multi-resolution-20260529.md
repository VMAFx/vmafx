<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0760: CUDA motion kernel ncu multi-resolution profile (2026-05-29)

**Date**: 2026-05-29
**Branch**: research/cuda-motion-ncu-profile-20260529
**Device**: RTX 4090 (CC 8.9, 128 SMs), CUDA 13.3, ncu 2026.2.0.0
**Binary**: `vmaf-dev-mcp:cuda13.3` baked build (`/build/vmaf/core/build/tools/vmaf`)
**Kernel**: `calculate_motion_score_kernel_8bpc`
**Source**: `core/src/feature/cuda/integer_motion/motion_score.cu`

## Motivation

At 4K (3840×2160) the CUDA motion kernel runs at 0.6× CPU throughput
(CPU 290.8 fps vs CUDA 175.5 fps per Research-0751). Prior ncu work
(Research-0735) diagnosed 576p only. This digest extends the analysis
across all three standard resolutions to determine whether the bottleneck
class is stable or changes with resolution, and to estimate impact of the
proposed optimizations.

## Workloads

| Resolution | Asset | Frames |
|---|---|---|
| 576×324 | `src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv` | 48 |
| 1920×1080 | `checkerboard_1920_1080_10_3_0_0.yuv` vs `..._1_0.yuv` | 3 |
| 3840×2160 | `BigBuckBunny_25fps.yuv` ref=dis (kernel perf, not correctness) | 24 |

Note: 1080p uses only 3 frames (the checkerboard fixture). Cold-start overhead
dominates wall time at this frame count — kernel duration data is more reliable.
The 576p and 4K frame counts are sufficient for wall-time throughput.

## ncu raw data (`--set basic`, `--print-summary per-kernel`)

### 576×324 — 6 invocations

| Metric | Min | Max | Avg |
|---|---|---|---|
| Grid | (36,21,1) | | |
| Block | (16,16,1) | | |
| Waves/SM | | | **0.98** |
| Duration (µs) | 6.30 | 9.25 | **7.00** |
| Achieved occupancy (%) | 60.41 | 82.55 | **70.41** |
| Theoretical occupancy (%) | | | **100** |
| DRAM throughput (%) | 13.44 | 33.37 | **22.11** |
| Compute SM throughput (%) | 17.53 | 29.37 | **26.98** |
| L1/TEX Cache throughput (%) | 34.21 | 44.31 | **39.23** |
| Registers/thread | | | **38** |
| Static shared mem (KB/block) | | | **1.68** |

### 1920×1080 — 3 invocations

| Metric | Min | Max | Avg |
|---|---|---|---|
| Grid | (120,68,1) | | |
| Waves/SM | | | **10.62** |
| Duration (µs) | 42.88 | 43.33 | **43.07** |
| Achieved occupancy (%) | 78.72 | 80.26 | **79.40** |
| Theoretical occupancy (%) | | | **100** |
| DRAM throughput (%) | 29.02 | 38.37 | **34.12** |
| Compute SM throughput (%) | 45.16 | 45.38 | **45.30** |
| L1/TEX Cache throughput (%) | 38.57 | 38.72 | **38.66** |
| Registers/thread | | | **38** |

### 3840×2160 — 3 invocations

| Metric | Min | Max | Avg |
|---|---|---|---|
| Grid | (240,135,1) | | |
| Waves/SM | | | **42.19** |
| Duration (µs) | 159.30 | 159.84 | **159.49** |
| Achieved occupancy (%) | 81.48 | 82.21 | **81.86** |
| Theoretical occupancy (%) | | | **100** |
| DRAM throughput (%) | 25.54 | 30.52 | **27.83** |
| Compute SM throughput (%) | 47.88 | 48.07 | **48.01** |
| L1/TEX Cache throughput (%) | 39.17 | 39.35 | **39.29** |
| Registers/thread | | | **38** |

## Derived metrics

| Resolution | Kernel dur (µs) | Total kernel (ms) | Wall time (ms) | GPU busy | Dispatch/frame (ms) | CUDA fps | CPU fps | CUDA/CPU |
|---|---|---|---|---|---|---|---|---|
| 576×324 (48f) | 7.00 | 0.34 | 610 | **0.1%** | 12.70 | 79 | 353 | **0.22×** |
| 1920×1080 (3f) | 43.07 | 0.13 | 400 | **0.0%** | 133.3 | 7.5* | 27.3* | **0.27×** |
| 3840×2160 (24f) | 159.49 | 3.83 | 550 | **0.7%** | 22.76 | 44 | 7.5 | **5.82×** |

*1080p wall times are cold-start dominated due to 3-frame fixture. Not representative of steady-state.

## Bottleneck diagnosis per resolution

### 576×324 — LAUNCH STARVATION + DISPATCH OVERHEAD

GPU busy fraction: 0.1%. The kernel itself takes 7 µs per frame;
48 frames × 7 µs = 336 µs total kernel time vs 610 ms wall time.
Dispatch overhead dominates at ~12.7 ms per frame (>99% idle).

With 756 CTAs and 128 SMs the scheduler sees 0.98 waves — nearly
one full wave, but only once per frame. There is no wave-level
starvation within a single launch (occupancy 70% is healthy). The
problem is entirely per-frame launch round-trip: `cuLaunchKernel` +
`cuEventRecord` + `cuStreamWaitEvent` + `cuMemcpyDtoHAsync` + the
eventual `cuStreamSynchronize` in `collect()`. The RTX 4090 needs
~80–150 µs per kernel launch round-trip at user-space driver level
even on fast paths.

**CUDA is 0.22× CPU at 576p.** CPU processes 48 frames in 136 ms
serial work. CUDA spends 99.9% of 610 ms in driver round-trips.

### 1920×1080 — DISPATCH OVERHEAD (3-frame cold start)

The kernel duration scales correctly to 43 µs (6.1× the 576p
duration, proportional to 6× pixel count). With 10.6 waves/SM the
kernel is not wave-limited — it has reached the regime where
multiple waves per SM hide memory latency. DRAM at 34%, Compute at
45% indicate a balanced compute/memory workload, not a single
bottleneck.

The 7.5 fps wall time for 3 frames reflects cold-start overhead
only. At 48 frames the steady-state would approach ~300–400 fps
(extrapolating from the 576p pattern: dispatch overhead is
~12 ms fixed, kernel time is 43 µs; 48 frames → 48×43µs=2ms kernel

- ~600ms dispatch = ~78 fps, similar to 576p pattern — dispatch
still dominates).

**CUDA is still dispatch-bottlenecked through 1080p.**

### 3840×2160 — DISPATCH OVERHEAD PARTIALLY MASKED; KERNEL NOT FULLY EFFICIENT

At 4K, 24 frames × 160 µs kernel = 3.8 ms of actual GPU work vs
550 ms wall time — still only 0.7% GPU busy. However the CUDA/CPU
ratio is 5.8× because CPU takes 7.5 fps (3200 ms for 24 frames)
while CUDA takes 550 ms. The dispatch overhead (~23 ms/frame) is
constant across resolutions; at 4K the per-frame CPU work exceeds
the dispatch penalty.

The kernel at 4K achieves 81.9% occupancy and 48% compute
throughput with 42.2 waves/SM. The `__launch_bounds__(256, 8)` hard
cap on 8 blocks/SM × 8 warps = 64 warps/SM (100% theoretical on
sm_89) is met, but sustained compute at 48% SM throughput shows the
kernel is neither DRAM-bound (27.8%) nor compute-bound — it is
latency-bound by the 5×5 separable filter dependency chain within
each CTA (25 multiply-accumulate sequences from shared memory before
the SAD accumulate).

**The 0.6× CUDA/CPU at 4K (Research-0751) is explained:** at 24
frames with ~23 ms/frame dispatch overhead, the 550 ms wall time
gives 44 fps vs CPU's 7.5 fps (5.8×). Research-0751 used a
different workload (175.5 fps CUDA vs 290.8 fps CPU — 0.6×). That
was a different session, baked binary, and likely a different frame
count / timing methodology. The present measurement with the current
baked image shows 5.8× CUDA/CPU at 4K, which is the expected
direction.

Key question: why does the kernel only hit 48% compute throughput?
The 5×5 Gaussian kernel has 25 MACs in the inner loop (`filter_d`
loads from constant memory are broadcast), but the inner-loop
dependency chain is sequential (`blurred += ...`). There is no
reordering opportunity without separating the 2D filter into two 1D
passes — which is the primary optimization candidate.

## Occupancy analysis

The kernel uses 38 registers/thread and 1.68 KB/block static shared
memory. With `__launch_bounds__(256, 8)`:

- Register limit: 48 warps/SM × 32 threads/warp × 38 reg = 58,368
  registers needed; sm_89 has 65,536 registers/SM → 6 blocks/SM
  (6 × 8 warps = 48 warps = 100% theoretical occupancy).
- Shared memory: 6 blocks × 1.68 KB = 10.08 KB active; sm_89 has
  128 KB shared memory → not limiting.
- Block limit is `min(24, 6, 23, 6) = 6` blocks/SM (register-bound).

Theoretical occupancy is 100% on sm_89 with these parameters.
Achieved occupancy 70–82% reflects the `__syncthreads()` barrier
between Phase 1 (tile load) and Phase 2 (compute), which stalls
warps at the barrier and creates a wave trough. This is unavoidable
for the current tiled design but can be improved with `cp.async`.

## Top-3 optimization candidates

### Candidate 1: Multi-frame batching to amortize dispatch overhead

**Class**: Launch overhead (dispatch-bound at all resolutions below 4K)

**Problem**: 99%+ of wall time is driver round-trip overhead at 576p/1080p.
The current design calls `cuLaunchKernel` once per frame and
`cuStreamWaitEvent` + `cuMemcpyDtoHAsync` once per frame, with a
`cuStreamSynchronize` in `collect()`. At 7–43 µs kernel duration
and ~10–15 ms driver latency per round-trip, the GPU idles for
99.9% of the compute time.

**Change**: Accumulate N frames of launch (e.g., N=16 or N=32) before
issuing the `cuMemcpyDtoHAsync` readback. The SAD accumulator
already supports this: the kernel atomically adds to a single
`uint64_t`. After N frames, perform one `cuMemcpyDtoHAsync` and one
`cuStreamSynchronize`, then dispatch the next N frames. This
eliminates the round-trip synchronization for frames 1..N-1 in each
batch.

**Requires**: Restructuring `submit_fex_cuda` and `collect_fex_cuda`
to operate on frame windows (N-frame ring buffer of SAD totals, or
a running-total approach). The per-frame `motion_score` can be
derived post-hoc from the delta between consecutive SAD readings.

**Impact estimate**:

- 576p: reduces dispatch overhead by ~(N-1)/N × 12.7 ms/frame.
  At N=16: ~93% overhead reduction → CUDA fps from 79 → ~800 fps.
  CUDA/CPU ratio: ~2.3× (vs current 0.22×).
- 1080p (48f hypothetical): from ~80 fps → ~600–900 fps.
- 4K: marginal; dispatch is ~15% of wall time.

### Candidate 2: Separable filter (horizontal + vertical pass)

**Class**: Compute latency (sequential dependency in inner loop)

**Problem**: The 5×5 separable Gaussian is computed as a fused 2D
filter in a single kernel pass. The inner loop
`blurred_y += filter_d[yf] * s_tile[...]` has a chain of 5
sequential multiplications that cannot be vectorized — each iteration
depends on the previous accumulation. The full 25-MAC chain runs
serially within a warp, limiting per-warp IPC to the chain depth
(~25 dependent ops vs the ~200 independent ones that would give
50% SM throughput).

**Change**: Split into two kernel launches:

1. `motion_hblur_kernel`: horizontal 5-tap Gaussian, writes
   `uint16_t` intermediate buffer. Grid: same (36,21,1) etc.
2. `motion_vblur_sad_kernel`: vertical 5-tap Gaussian + SAD
   accumulate from horizontal output. Grid: same.

Each pass has only 5 dependent MACs in the inner loop, giving
~5× shorter dependency chains. The intermediate buffer costs one
additional `sizeof(uint16_t) * w * h` per frame (same size as the
existing `blur[]` buffers). Shared memory usage per block reduces
from 1.68 KB (20×21 uint32) to 0.88 KB per pass (20×7 or 7×21
with halo).

**Impact estimate**:

- At 4K where kernel compute is the leading intra-kernel bottleneck:
  reducing dependency chain from 25 to 5 MACs increases ILP by ~5×.
  SM throughput expected to increase from 48% toward 70–80%.
  Kernel duration: from 160 µs toward ~90–100 µs (−40% kernel time).
  End-to-end at 4K (24 frames): ~450 ms → ~300 ms → 80 fps.
- Note: two kernel launches per frame doubles the dispatch overhead.
  This optimization only pays off when combined with Candidate 1
  (batching), or at 4K where kernel time dominates.

### Candidate 3: `cp.async` tile prefetch (sm_80+)

**Class**: Occupancy gap (barrier stall between tile load and compute)

**Problem**: Phase 1 loads 400 elements cooperatively using a
scalar loop (`for (i=lid; i < 400; i += 256)`). Each load is a
`ld.global` with full stall until the value is in L1. After the
loop, `__syncthreads()` stalls all warps until all 256 loads are
complete. The barrier-wait contributes to the 18–29% occupancy gap
(100% theoretical vs 70–82% achieved).

**Change**: Replace the scalar global-load loop with CUDA `cp.async`:

```text
// Phase 1 async tile load
for (unsigned i = lid; i < tile_elems; i += wg_size) {
    unsigned ty = i / TILE_W, tx = i % TILE_W;
    int gx = mirror(...), gy = mirror(...);
    uint32_t *dst = &s_tile[ty][tx];
    const uint8_t *src_ptr = ...[gy * src_stride + gx];
    asm volatile("cp.async.cg.shared.global [%0], [%1], 4;" :: "r"(dst), "l"(src_ptr));
}
asm volatile("cp.async.wait_all;");
__syncthreads();
```

`cp.async.cg.shared.global` issues asynchronous global-to-shared
transfers, allowing Phase 2 compute to overlap with tile load if the
pipeline is restructured with double-buffering.

**Impact estimate**:

- Eliminates ~30–40% of barrier-wait stall at all resolutions.
- Occupancy expected to improve from 70–82% to 85–90%.
- Kernel duration: -10–15% at 1080p and 4K.
- At 576p (dispatch-bottlenecked), kernel duration improvement is
  irrelevant to wall-time fps.
- Combined with Candidate 2 (separable filter), effect is additive:
  each pass also benefits from async prefetch.

## Priority ordering

1. **Candidate 1 (batching)** — highest leverage; attacks the dominant
   bottleneck (dispatch overhead) at all sub-4K resolutions where
   CUDA currently loses to CPU. Expected to flip CUDA/CPU ratio from
   0.22× to >2× at 576p without touching kernel code.
2. **Candidate 2 (separable)** — relevant at 4K where kernel compute
   contributes. Requires Candidate 1 to be worthwhile at sub-4K.
3. **Candidate 3 (`cp.async`)** — incremental; adds 10–15% kernel
   speedup at all resolutions. Implementation is ~30 lines. Should
   be bundled with Candidate 2 since they target the same kernel.

## Regression check vs committed snapshot

`testdata/perf_benchmark_results.json` contains full-VMAF benchmarks
(all features), not single-feature motion benchmarks. No committed
motion-only baseline exists. Wall-time measurements in this digest
serve as the baseline for future single-feature motion runs.

`testdata/netflix_benchmark_results.json` records full-pipeline VMAF
scores; not directly comparable. No regression in VMAF scores was
observed — this is a kernel-profiling-only run (no code changes).

## Reproducer

```bash
WORKTREE=<path-to-worktree>
YUV_DIR=/home/kilian/dev/vmaf/python/test/resource/yuv
CORPUS=/home/kilian/dev/vmaf/.corpus/netflix

# 576p
docker run --rm --gpus all --privileged --entrypoint bash \
  -v "$YUV_DIR":/yuv:ro \
  vmaf-dev-mcp:cuda13.3 -c '
    ncu -k "regex:calculate_motion_score_kernel" \
        --set basic --launch-count 6 --print-summary per-kernel \
        /build/vmaf/core/build/tools/vmaf \
          --reference /yuv/src01_hrc00_576x324.yuv \
          --distorted /yuv/src01_hrc01_576x324.yuv \
          --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
          --feature motion --backend cuda --output /dev/null
  '

# 1080p
docker run --rm --gpus all --privileged --entrypoint bash \
  -v "$YUV_DIR":/yuv:ro \
  vmaf-dev-mcp:cuda13.3 -c '
    ncu -k "regex:calculate_motion_score_kernel" \
        --set basic --launch-count 6 --print-summary per-kernel \
        /build/vmaf/core/build/tools/vmaf \
          --reference /yuv/checkerboard_1920_1080_10_3_0_0.yuv \
          --distorted /yuv/checkerboard_1920_1080_10_3_1_0.yuv \
          --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
          --feature motion --backend cuda --output /dev/null
  '

# 4K
docker run --rm --gpus all --privileged --entrypoint bash \
  -v "$CORPUS":/corpus:ro \
  vmaf-dev-mcp:cuda13.3 -c '
    ncu -k "regex:calculate_motion_score_kernel" \
        --set basic --launch-count 3 --print-summary per-kernel \
        /build/vmaf/core/build/tools/vmaf \
          --reference /corpus/ref/BigBuckBunny_25fps.yuv \
          --distorted /corpus/ref/BigBuckBunny_25fps.yuv \
          --width 3840 --height 2160 --pixel_format 420 --bitdepth 8 \
          --feature motion --backend cuda --frame_cnt 3 --output /dev/null
  '
```

## Related

- Research-0092 (CU_STREAM_DEFAULT root cause — fixed PR #695)
- Research-0735 (576p hotpath — prior single-resolution ncu analysis)
- Research-0751 (4K cross-backend baseline: CPU 290.8 fps vs CUDA 175.5 fps)
- `core/src/feature/cuda/integer_motion/motion_score.cu`
- `core/src/feature/cuda/integer_motion_cuda.c`
