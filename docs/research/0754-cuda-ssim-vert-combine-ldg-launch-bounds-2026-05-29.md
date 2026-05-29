# Research-0754 — CUDA SSIM vert_combine: __ldg() + __launch_bounds__ + pinned-host leak

**Date:** 2026-05-29
**ADR:** [ADR-0754](../adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md)
**Status:** Static analysis complete; live ncu A/B pending GPU access.

## Motivation

Three findings from the `calculate_ssim_vert_combine` review:

- F2: 55 inner-loop global loads from 5 intermediate buffers without the
  read-only cache path — struct-by-value argument hides pointer from
  compiler non-aliased-load analysis.
- F4: `__launch_bounds__` absent on a 128-thread kernel — register allocation
  left unconstrained relative to the actual launch config.
- F6: `close_fex_cuda` leaked one page of CUDA pinned host memory per
  `vmaf_close()` cycle (host_pinned NULLed by readback_free, never freed).

## Baseline (static analysis / analogy)

From ADR-0743 (VIF filter1d, same GPU, same strategy):

- Before `__ldg()`: 17-tap horizontal pass at 1080p showed measurable L2
  pressure; `__ldg()` on 7 read-only tmp buffers provided L2-pressure relief
  at >= 1080p where combined tmp footprint exceeds L2 capacity.
- `__launch_bounds__(128, 10)` on the 17-tap kernel: registers dropped
  56 -> 48 per thread (sm_89), theoretical occupancy 75% -> 83.3%.

For `vert_combine`:

- 5 intermediate buffers x 11 taps = 55 global loads per output pixel.
  All 5 buffers are write-once (horizontal pass) / read-many (vertical pass),
  making them ideal for `__ldg()` routing through the L1 read-only cache.
- 16x8 = 128 threads/block; `__launch_bounds__(128)` without `min_blocks`
  sets the register budget upper bound only — a conservative hint that
  cannot regress occupancy.

## Expected deltas (qualitative, to be confirmed by ncu)

| Metric            | Expected direction | Confidence |
| ----------------- | ------------------ | ---------- |
| DRAM throughput   | decrease (fewer L2 misses) | Medium at >= 1080p |
| L1 read-only hit rate | increase    | High (55 loads now eligible) |
| Kernel duration   | neutral to slight decrease | Low (small kernel, wave-limited at 576p) |
| Register count    | neutral to slight decrease | Medium (F4 hint) |

At 576x324 (the Netflix golden fixture), `vert_combine` is likely
wave-limited (< 1 wave across 128 SMs for the full frame) and the
cache effect will be small. The measurement matters most at 1080p+.

## Correctness verification

The ADR-0214 `places=4` cross-backend gate (CPU vs CUDA) is the primary
correctness gate. `__ldg()` is semantically equivalent to a regular load
for data that is not written during the kernel — it only changes the cache
routing, not the computed values. `__launch_bounds__` affects register
allocation only. F6 is a cleanup-path fix that does not affect any computed
score.

## ncu reproducer commands

```bash
# Baseline (master, before this PR):
docker run --rm --gpus all -v $(pwd):/workspace -w /workspace \
  vmaf-dev-mcp:cuda13.3 bash -c '
    ncu --target-processes all \
        --section MemoryWorkloadAnalysis \
        --section LaunchStats \
        --kernel-name calculate_ssim_vert_combine \
        /usr/local/bin/vmaf \
          --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
          --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
          --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
          --feature float_ssim --backend cuda
  '

# Optimized (this branch):
# Same command against the rebuilt vmaf binary from build-opt/tools/vmaf.
```

## Decision on F1/F3 follow-up

F1 (AoS to SoA buffer restructure) + F3 (matching signature change) are
deferred. Rationale: at 576p the kernel is wave-limited; any gain from F2
will be small and the gain from a full AoS->SoA restructure even smaller.
At 1080p the F2 cache routing change alone should capture most of the
available L2-pressure win. Revisit F1 only if live ncu shows the L1
read-only hit rate is still materially below 90% after F2 ships.

## Note on integer_psnr_cuda.c

The same `readback_free` / `host_free` gap exists in `integer_psnr_cuda.c`
(also confirmed by code inspection: `readback_free` called in close without
preceding `host_free`). That file uses multiple `rb[]` slots per extractor.
Fix is straightforward but in a different file; named explicitly for the
next CUDA cleanup PR.
